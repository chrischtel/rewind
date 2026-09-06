#include "rewind/net/lobby.h"

#include "rewind/core/log.h"
#include "rewind/sim/serialize.h"

#include <algorithm>
#include <cstring>

namespace rw::net {

namespace {
constexpr u32 kMagic = 0x424C5752u; // "RWLB"
constexpr u64 kResendMs = 250;
constexpr u64 kTimeoutMs = 6000;

void put_address(Writer& w, const Address& a) {
    w.str(a.host);
    w.pod<u16>(a.port);
}
Address get_address(Reader& r) {
    Address a;
    r.str(a.host);
    a.port = r.pod<u16>();
    return a;
}
} // namespace

bool Lobby::is_lobby_packet(const std::vector<u8>& bytes) {
    if (bytes.size() < 5) return false;
    u32 magic = 0;
    std::memcpy(&magic, bytes.data(), 4);
    return magic == kMagic;
}

Lobby::Lobby(AddressedTransport& transport) : transport_(transport) {}

void Lobby::host(const std::string& name, u32 max_players) {
    max_players = std::clamp<u32>(max_players, 1, kMaxPlayers);
    players_.assign(max_players, LobbyPlayer{});
    players_[0].name = name;
    players_[0].present = true;
    name_ = name;
    host_ = true;
    local_slot_ = 0;
    ready_ = false;
    state_ = State::Waiting;
    error_.clear();
}

void Lobby::join(const Address& host_address, const std::string& name) {
    host_address_ = host_address;
    name_ = name;
    host_ = false;
    ready_ = false;
    players_.clear();
    state_ = State::Joining;
    joined_ms_ = 0;
    error_.clear();
}

void Lobby::leave() {
    if (state_ == State::Waiting || state_ == State::Starting || state_ == State::Joining) {
        Writer w;
        w.pod(kMagic);
        w.pod<u8>(static_cast<u8>(Msg::Leave));
        w.pod<u8>(static_cast<u8>(local_slot_));
        if (host_) {
            for (const LobbyPlayer& p : players_)
                if (p.present && p.address.valid()) transport_.send_to(w.bytes(), p.address);
        } else {
            transport_.send_to(w.bytes(), host_address_);
        }
    }
    state_ = State::Idle;
    players_.clear();
}

u32 Lobby::player_count() const {
    u32 n = 0;
    for (const LobbyPlayer& p : players_) n += p.present ? 1 : 0;
    return n;
}

bool Lobby::all_ready() const {
    if (player_count() < 1) return false;
    for (const LobbyPlayer& p : players_)
        if (p.present && !p.ready) return false;
    return true;
}

u32 Lobby::countdown_ms(u64 now_ms) const {
    if (state_ != State::Starting || start_at_ms_ <= now_ms) return 0;
    return static_cast<u32>(start_at_ms_ - now_ms);
}

void Lobby::fail(const std::string& why) {
    error_ = why;
    state_ = State::Failed;
    RW_LOG_ERROR("lobby", "%s", why.c_str());
}

// ---- messages -----------------------------------------------------------------------

void Lobby::send_join() {
    Writer w;
    w.pod(kMagic);
    w.pod<u8>(static_cast<u8>(Msg::Join));
    w.str(name_);
    transport_.send_to(w.bytes(), host_address_);
}

void Lobby::send_ready() {
    Writer w;
    w.pod(kMagic);
    w.pod<u8>(static_cast<u8>(Msg::Ready));
    w.pod<u8>(static_cast<u8>(local_slot_));
    w.pod<u8>(ready_ ? 1 : 0);
    transport_.send_to(w.bytes(), host_address_);
}

void Lobby::send_state(u64 now_ms) {
    Writer w;
    w.pod(kMagic);
    w.pod<u8>(static_cast<u8>(Msg::State));
    w.pod<u8>(static_cast<u8>(players_.size()));
    for (const LobbyPlayer& p : players_) {
        w.pod<u8>(p.present ? 1 : 0);
        w.pod<u8>(p.ready ? 1 : 0);
        w.str(p.name);
        put_address(w, p.address);
    }
    for (usize i = 0; i < players_.size(); ++i)
        if (i != local_slot_ && players_[i].present && players_[i].address.valid()) transport_.send_to(w.bytes(), players_[i].address);
    (void)now_ms;
}

void Lobby::send_start() {
    Writer w;
    w.pod(kMagic);
    w.pod<u8>(static_cast<u8>(Msg::Start));
    w.pod<u64>(seed_);
    w.pod<u64>(start_at_ms_);
    for (usize i = 0; i < players_.size(); ++i)
        if (i != local_slot_ && players_[i].present && players_[i].address.valid()) transport_.send_to(w.bytes(), players_[i].address);
}

void Lobby::handle(const std::vector<u8>& bytes, const Address& from, u64 now_ms) {
    Reader r(bytes);
    if (r.pod<u32>() != kMagic) return;
    const Msg msg = static_cast<Msg>(r.pod<u8>());

    if (host_) {
        switch (msg) {
        case Msg::Join: {
            std::string name;
            r.str(name);
            // Same address again: same slot (their WELCOME may have been lost).
            u32 slot = ~0u;
            for (usize i = 0; i < players_.size(); ++i)
                if (players_[i].present && players_[i].address == from) slot = static_cast<u32>(i);
            if (slot == ~0u && state_ == State::Waiting)
                for (usize i = 0; i < players_.size(); ++i)
                    if (!players_[i].present) {
                        slot = static_cast<u32>(i);
                        players_[i] = LobbyPlayer{name, from, true, false, now_ms};
                        RW_LOG_INFO("lobby", "%s joined as player %u from %s:%u", name.c_str(), slot, from.host.c_str(), from.port);
                        break;
                    }
            if (slot == ~0u) return; // full or already started: silence tells them
            players_[slot].last_seen_ms = now_ms;
            Writer w;
            w.pod(kMagic);
            w.pod<u8>(static_cast<u8>(Msg::Welcome));
            w.pod<u8>(static_cast<u8>(slot));
            w.pod<u8>(static_cast<u8>(players_.size()));
            transport_.send_to(w.bytes(), from);
            send_state(now_ms);
            break;
        }
        case Msg::Ready: {
            const u8 slot = r.pod<u8>();
            const bool ready = r.pod<u8>() != 0;
            if (slot < players_.size() && players_[slot].present && players_[slot].address == from) {
                players_[slot].ready = ready;
                players_[slot].last_seen_ms = now_ms;
            }
            break;
        }
        case Msg::Leave: {
            const u8 slot = r.pod<u8>();
            if (slot < players_.size() && players_[slot].address == from && state_ == State::Waiting) {
                players_[slot] = LobbyPlayer{};
                send_state(now_ms);
            }
            break;
        }
        default: break;
        }
        return;
    }

    // Client side.
    switch (msg) {
    case Msg::Welcome: {
        const u8 slot = r.pod<u8>();
        const u8 count = r.pod<u8>();
        if (!r.ok() || count == 0 || slot >= count) return;
        if (state_ == State::Joining) {
            local_slot_ = slot;
            players_.assign(count, LobbyPlayer{});
            state_ = State::Waiting;
            joined_ms_ = now_ms;
            RW_LOG_INFO("lobby", "joined as player %u of %u", slot, count);
        }
        break;
    }
    case Msg::State: {
        if (state_ != State::Waiting && state_ != State::Starting) return;
        const u8 count = r.pod<u8>();
        if (count != players_.size()) return;
        for (u8 i = 0; i < count; ++i) {
            LobbyPlayer& p = players_[i];
            p.present = r.pod<u8>() != 0;
            p.ready = r.pod<u8>() != 0;
            r.str(p.name);
            p.address = get_address(r);
        }
        if (!r.ok()) return;
        // The host's own slot: the address we joined is the one that works from here.
        for (u8 i = 0; i < count; ++i)
            if (players_[i].present && !players_[i].address.valid()) players_[i].address = host_address_;
        joined_ms_ = now_ms;
        break;
    }
    case Msg::Start: {
        const u64 seed = r.pod<u64>();
        const u64 remaining = r.pod<u64>();
        if (!r.ok() || (state_ != State::Waiting && state_ != State::Starting)) return;
        if (state_ != State::Starting) {
            seed_ = seed;
            start_at_ms_ = now_ms + remaining;
            state_ = State::Starting;
            RW_LOG_INFO("lobby", "starting in %llu ms, seed %llx", static_cast<unsigned long long>(remaining),
                        static_cast<unsigned long long>(seed));
        }
        joined_ms_ = now_ms;
        break;
    }
    default: break;
    }
}

void Lobby::update(u64 now_ms) {
    if (state_ == State::Idle || state_ == State::Failed || state_ == State::Started) return;

    Address from;
    while (transport_.receive_from(scratch_, from))
        if (is_lobby_packet(scratch_)) handle(scratch_, from, now_ms);

    const bool due = now_ms - last_send_ms_ >= kResendMs;
    if (host_) {
        // Drop players that went silent (not once the countdown runs).
        if (state_ == State::Waiting)
            for (usize i = 1; i < players_.size(); ++i)
                if (players_[i].present && now_ms - players_[i].last_seen_ms > kTimeoutMs) {
                    RW_LOG_WARN("lobby", "player %zu (%s) timed out", i, players_[i].name.c_str());
                    players_[i] = LobbyPlayer{};
                }
        if (due) {
            send_state(now_ms);
            if (state_ == State::Starting) {
                // The countdown message carries the time left, so late copies still agree.
                Writer w;
                w.pod(kMagic);
                w.pod<u8>(static_cast<u8>(Msg::Start));
                w.pod<u64>(seed_);
                w.pod<u64>(start_at_ms_ > now_ms ? start_at_ms_ - now_ms : 0);
                for (usize i = 1; i < players_.size(); ++i)
                    if (players_[i].present) transport_.send_to(w.bytes(), players_[i].address);
            }
            last_send_ms_ = now_ms;
        }
    } else {
        if (state_ == State::Joining) {
            if (joined_ms_ == 0) joined_ms_ = now_ms;
            if (now_ms - joined_ms_ > kTimeoutMs * 2) {
                fail("no answer from " + host_address_.host + ":" + std::to_string(host_address_.port));
                return;
            }
            if (due) {
                send_join();
                last_send_ms_ = now_ms;
            }
        } else if (due) {
            send_ready(); // doubles as the heartbeat
            last_send_ms_ = now_ms;
            if (state_ == State::Waiting && now_ms - joined_ms_ > kTimeoutMs) fail("lost the host");
        }
    }
    if (state_ == State::Starting && now_ms >= start_at_ms_) {
        state_ = State::Started;
        RW_LOG_INFO("lobby", "started: %u players, local slot %u", player_count(), local_slot_);
    }
}

void Lobby::set_ready(bool ready) {
    ready_ = ready;
    if (local_slot_ < players_.size()) players_[local_slot_].ready = ready;
    if (!host_ && (state_ == State::Waiting || state_ == State::Starting)) send_ready();
}

bool Lobby::start(u64 now_ms, u32 countdown_ms, u64 seed) {
    if (!host_ || state_ != State::Waiting || !all_ready()) return false;
    seed_ = seed ? seed : (now_ms * 0x9E3779B97F4A7C15ull) | 1u;
    start_at_ms_ = now_ms + countdown_ms;
    state_ = State::Starting;
    // The final roster is frozen here; late joiners see silence.
    Writer w;
    w.pod(kMagic);
    w.pod<u8>(static_cast<u8>(Msg::Start));
    w.pod<u64>(seed_);
    w.pod<u64>(countdown_ms);
    for (usize i = 1; i < players_.size(); ++i)
        if (players_[i].present) transport_.send_to(w.bytes(), players_[i].address);
    last_send_ms_ = now_ms;
    return true;
}

std::vector<Address> Lobby::peer_addresses() const {
    std::vector<Address> out;
    for (usize i = 0; i < players_.size(); ++i)
        if (i != local_slot_ && players_[i].present) out.push_back(players_[i].address);
    return out;
}

void Lobby::connect_peers(AddressedTransport& transport) const {
    transport.clear_peers();
    for (const Address& a : peer_addresses()) transport.add_peer(a);
}

SessionConfig Lobby::session_config(u32 input_delay) const {
    SessionConfig cfg;
    // Slots stay as assigned, so absent slots would be silent players; the
    // host only starts with everyone present in the lowest slots.
    cfg.player_count = std::max<u32>(1, static_cast<u32>(players_.size()));
    cfg.local_player = local_slot_;
    cfg.input_delay = input_delay;
    return cfg;
}

} // namespace rw::net
