#include "rewind/net/session.h"

#include "rewind/core/log.h"
#include "rewind/core/profiler.h"
#include "rewind/sim/game.h"
#include "rewind/sim/serialize.h"
#include "rewind/sim/timeline.h"

#include <algorithm>
#include <limits>

namespace rw::net {

namespace {

constexpr u8 kMagic = 0xA7;
constexpr u8 kVersion = 1;
constexpr Tick kNoRollback = std::numeric_limits<Tick>::max();
constexpr usize kMaxFramesPerPacket = 64;

} // namespace

// ---- wire format ------------------------------------------------------------------

void encode_packet(const Packet& p, std::vector<u8>& out) {
    Writer w;
    w.pod(kMagic);
    w.pod(kVersion);
    w.pod(p.player);
    w.pod<u8>(static_cast<u8>(std::min<usize>(p.frames.size(), kMaxFramesPerPacket)));
    w.pod(p.start_tick);
    for (usize i = 0; i < p.frames.size() && i < kMaxFramesPerPacket; ++i) w.pod(p.frames[i]);
    w.pod(p.ack);
    w.pod(p.time_ms);
    w.pod(p.echo_ms);
    w.pod<u8>(p.has_sync ? 1 : 0);
    if (p.has_sync) {
        w.pod(p.sync_tick);
        w.pod(p.sync_hash);
    }
    out = std::move(w.bytes());
}

bool decode_packet(const std::vector<u8>& bytes, Packet& out) {
    Reader r(bytes);
    if (r.pod<u8>() != kMagic || r.pod<u8>() != kVersion) return false;
    out = Packet{};
    out.player = r.pod<u8>();
    const u8 count = r.pod<u8>();
    out.start_tick = r.pod<u32>();
    out.frames.resize(count);
    for (u8 i = 0; i < count; ++i) out.frames[i] = r.pod<InputFrame>();
    out.ack = r.pod<u32>();
    out.time_ms = r.pod<u32>();
    out.echo_ms = r.pod<u32>();
    out.has_sync = r.pod<u8>() != 0;
    if (out.has_sync) {
        out.sync_tick = r.pod<u32>();
        out.sync_hash = r.pod<u64>();
    }
    return r.ok() && out.player < kMaxPlayers;
}

// ---- per-player input history -------------------------------------------------------

void Session::PlayerInputs::append(Tick t, const InputFrame& f) {
    if (t != known_until) return; // only contiguous growth; gaps are filled by later redundant packets
    frames.push_back(f);
    ++known_until;
    last = f;
}

void Session::PlayerInputs::trim(Tick keep_from) {
    if (keep_from <= base) return;
    const usize drop = static_cast<usize>(std::min(keep_from, known_until) - base);
    frames.erase(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(drop));
    base += drop;
}

// ---- session -----------------------------------------------------------------------

Session::Session(const SessionConfig& config, Transport& transport)
    : config_(config), transport_(transport), rollback_from_(kNoRollback) {
    if (config_.player_count < 1) config_.player_count = 1;
    if (config_.player_count > kMaxPlayers) config_.player_count = kMaxPlayers;
    if (config_.local_player >= config_.player_count) config_.local_player = 0;
    players_.resize(config_.player_count);
    acks_.assign(config_.player_count, 0);
    heard_.assign(config_.player_count, false);

    // Nobody can react before the input delay has passed: those ticks are idle
    // for everyone, on every peer, so all of them agree without talking.
    for (Tick t = 0; t < config_.input_delay; ++t) players_[config_.local_player].append(t, InputFrame{});
}

Tick Session::confirmed_tick() const {
    Tick t = std::numeric_limits<Tick>::max();
    for (const PlayerInputs& p : players_) t = std::min(t, p.known_until);
    return t;
}

void Session::add_local(Tick tick, const InputFrame& input) {
    PlayerInputs& me = players_[config_.local_player];
    while (me.known_until <= tick) me.append(me.known_until, input);
}

InputSet Session::inputs_for(Tick tick, bool& predicted) const {
    InputSet set;
    set.count = static_cast<u8>(config_.player_count);
    predicted = false;
    for (u32 p = 0; p < config_.player_count; ++p) {
        const PlayerInputs& pi = players_[p];
        if (pi.known(tick)) {
            set.players[p] = pi.at(tick);
        } else {
            set.players[p] = pi.last; // prediction: keep doing what they did last
            if (p != config_.local_player) predicted = true;
        }
    }
    return set;
}

void Session::receive(Timeline& timeline, u64 now_ms) {
    Packet packet;
    while (transport_.receive(scratch_)) {
        if (!decode_packet(scratch_, packet)) continue;
        if (packet.player == config_.local_player || packet.player >= config_.player_count) continue;
        ++stats_.packets_received;
        stats_.connected = true;
        if (!heard_[packet.player]) {
            heard_[packet.player] = true;
            ++stats_.connected_players;
        }
        last_remote_time_ms_ = packet.time_ms;
        if (packet.echo_ms != 0) {
            const u32 now32 = static_cast<u32>(now_ms);
            const float sample = static_cast<float>(now32 - packet.echo_ms);
            stats_.rtt_ms = stats_.rtt_ms == 0 ? sample : stats_.rtt_ms * 0.9f + sample * 0.1f;
        }
        acks_[packet.player] = std::max<Tick>(acks_[packet.player], packet.ack);
        // Resend from what the slowest peer still lacks.
        Tick lowest = std::numeric_limits<Tick>::max();
        for (u32 i = 0; i < config_.player_count; ++i)
            if (i != config_.local_player) lowest = std::min(lowest, acks_[i]);
        remote_ack_ = lowest == std::numeric_limits<Tick>::max() ? 0 : lowest;

        PlayerInputs& pi = players_[packet.player];
        for (usize i = 0; i < packet.frames.size(); ++i) {
            const Tick t = packet.start_tick + i;
            if (t < pi.known_until) continue; // already have it
            if (t > pi.known_until) break;    // gap: a later packet repeats these
            const InputFrame& f = packet.frames[i];
            if (t < timeline.head() && timeline.in_history(t) && !(timeline.inputs_at(t)[packet.player] == f)) {
                rollback_from_ = std::min(rollback_from_, t); // prediction was wrong
            }
            pi.append(t, f);
        }
        if (packet.has_sync) pending_syncs_.emplace_back(packet.sync_tick, packet.sync_hash);
    }
}

void Session::send(Timeline& timeline, u64 now_ms) {
    (void)timeline;
    const PlayerInputs& me = players_[config_.local_player];
    Packet p;
    p.player = static_cast<u8>(config_.local_player);
    Tick from = std::max(me.base, remote_ack_);
    if (me.known_until > from + config_.redundancy) from = me.known_until - config_.redundancy;
    p.start_tick = static_cast<u32>(from);
    for (Tick t = from; t < me.known_until; ++t) p.frames.push_back(me.at(t));

    Tick ack = std::numeric_limits<Tick>::max();
    for (u32 i = 0; i < config_.player_count; ++i)
        if (i != config_.local_player) ack = std::min(ack, players_[i].known_until);
    p.ack = ack == std::numeric_limits<Tick>::max() ? 0 : static_cast<u32>(ack);
    p.time_ms = static_cast<u32>(now_ms) | 1u; // never 0, which means "no echo"
    p.echo_ms = last_remote_time_ms_;

    if (ticks_since_sync_ >= config_.sync_interval) {
        // Fingerprint of the newest state that every peer has built from confirmed input.
        const Tick confirmed = std::min(confirmed_tick(), timeline.head());
        if (confirmed > timeline.oldest()) {
            p.has_sync = true;
            p.sync_tick = static_cast<u32>(confirmed);
            p.sync_hash = timeline.hash_at(confirmed);
            ticks_since_sync_ = 0;
        }
    }

    encode_packet(p, scratch_);
    transport_.send(scratch_);
    ++stats_.packets_sent;
}

void Session::check_syncs(Timeline& timeline) {
    const Tick confirmed = std::min(confirmed_tick(), timeline.head());
    while (!pending_syncs_.empty()) {
        const auto [tick, hash] = pending_syncs_.front();
        if (tick > confirmed) break; // not there yet; keep it
        pending_syncs_.pop_front();
        if (!timeline.in_history(tick)) continue;
        if (timeline.hash_at(tick) != hash && !stats_.desync) {
            stats_.desync = true;
            stats_.desync_tick = tick;
            RW_LOG_ERROR("net", "desync: peers disagree on the state at tick %llu",
                         static_cast<unsigned long long>(tick));
        }
    }
}

u32 Session::step(Timeline& timeline, Game& game, const InputFrame& local, u32 wanted_ticks, EventQueue* events,
                  u64 now_ms) {
    RW_PROFILE_SCOPE("net");
    receive(timeline, now_ms);

    // Fix what we guessed wrong, then re-run from there. All of it is history
    // the timeline already holds; only the inputs change.
    if (rollback_from_ != kNoRollback && rollback_from_ < timeline.head()) {
        for (Tick t = rollback_from_; t < timeline.head(); ++t) {
            bool predicted = false;
            timeline.set_inputs(t, inputs_for(t, predicted));
        }
        const u32 rerun = timeline.rollback(game, rollback_from_);
        ++stats_.rollbacks;
        stats_.rolled_back_ticks += rerun;
    }
    rollback_from_ = kNoRollback;

    u32 advanced = 0;
    for (u32 i = 0; i < wanted_ticks; ++i) {
        const Tick tick = timeline.head();
        add_local(tick + config_.input_delay, local);

        Tick remote_known = std::numeric_limits<Tick>::max();
        for (u32 p = 0; p < config_.player_count; ++p)
            if (p != config_.local_player) remote_known = std::min(remote_known, players_[p].known_until);
        if (config_.player_count > 1 && tick >= remote_known + config_.max_rollback) {
            ++stats_.stalled_frames; // running further would exceed what we can roll back
            break;
        }

        bool predicted = false;
        const InputSet inputs = inputs_for(tick, predicted);
        timeline.advance(game, inputs, events);
        ++advanced;
        ++ticks_since_sync_;
    }

    send(timeline, now_ms);
    check_syncs(timeline);

    // Bookkeeping and stats.
    Tick remote_known = timeline.head();
    for (u32 p = 0; p < config_.player_count; ++p)
        if (p != config_.local_player) remote_known = std::min(remote_known, players_[p].known_until);
    stats_.remote_confirmed = remote_known;
    stats_.predicted_ticks = static_cast<u32>(timeline.head() > remote_known ? timeline.head() - remote_known : 0);

    const Tick keep = std::min(timeline.oldest(), remote_ack_);
    for (PlayerInputs& p : players_) p.trim(keep);
    return advanced;
}

} // namespace rw::net
