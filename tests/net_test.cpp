// Headless checks for rollback netcode: the wire format, two sessions
// talking over a lossy, laggy loopback, stalls, and desync detection.

#include <rewind/net/lobby.h>
#include <rewind/net/session.h>
#include <rewind/net/transport.h>
#include <rewind/sim/game.h>
#include <rewind/sim/serialize.h>
#include <rewind/sim/timeline.h>

#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

using namespace rw;

namespace {

int failures = 0;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

/// Two players pushing numbers around. Every input of every player changes
/// the state, so a wrong prediction always shows.
struct Arena final : Game {
    i64 pos[2] = {0, 0};
    u64 mix = 1;
    bool hidden_bug = false;

    void tick(const TickContext& c) override {
        for (u32 p = 0; p < 2; ++p) {
            const InputFrame& in = c.player(p);
            pos[p] += in.axis_x + (in.down(Button::A) ? 1000 : 0);
            mix = mix * 6364136223846793005ull + static_cast<u64>(in.buttons) + static_cast<u64>(p);
        }
        if (hidden_bug && c.tick > 40) mix += 1;
    }
    void reflect(Visitor& v) override {
        v.field("p0", pos[0]);
        v.field("p1", pos[1]);
        v.field("mix", mix);
    }
    void render(Renderer2D&, float) override {}
};

/// Deterministic pseudo-random input per player and tick, changing often
/// enough that "same as last time" predictions are frequently wrong.
InputFrame input_for(u32 player, Tick tick) {
    InputFrame in;
    const u64 h = (tick + 1) * 0x9E3779B97F4A7C15ull ^ (player + 7) * 0xC2B2AE3D27D4EB4Full;
    if ((h >> 7) % 3 == 0) in.axis_x = static_cast<i16>((h >> 16) % 2000) - 1000;
    if ((h >> 21) % 11 == 0) in.set(Button::A, true);
    return in;
}

struct Peer {
    Arena game;
    Timeline timeline;
    net::Session session;
    Peer(const net::SessionConfig& cfg, net::Transport& t) : session(cfg, t) { timeline.reset(game); }
};

/// Runs both peers for `frames` frames of 16 ms with the given network.
/// Returns the tick both agree is confirmed.
Tick run_pair(Peer& a, Peer& b, net::LoopbackPair& link, int frames, bool b_silent = false) {
    for (int f = 0; f < frames; ++f) {
        a.session.step(a.timeline, a.game, input_for(0, a.timeline.head()), 1, nullptr, link.now_ms());
        if (!b_silent) b.session.step(b.timeline, b.game, input_for(1, b.timeline.head()), 1, nullptr, link.now_ms());
        link.advance(16);
    }
    return std::min(a.session.confirmed_tick(), b.session.confirmed_tick());
}

bool states_agree(Peer& a, Peer& b, Tick upto) {
    // Every confirmed tick must carry the same fingerprint on both sides.
    for (Tick t = std::max(a.timeline.oldest(), b.timeline.oldest()); t <= upto && t <= a.timeline.head() && t <= b.timeline.head(); ++t) {
        if (a.timeline.hash_at(t) != b.timeline.hash_at(t)) {
            std::printf("  mismatch at tick %llu\n", static_cast<unsigned long long>(t));
            return false;
        }
    }
    return true;
}

void test_packet_round_trip() {
    net::Packet p;
    p.player = 1;
    p.start_tick = 1234;
    for (int i = 0; i < 5; ++i) {
        InputFrame f;
        f.buttons = static_cast<u32>(i * 3);
        f.axis_x = static_cast<i16>(i * 100 - 200);
        p.frames.push_back(f);
    }
    p.ack = 99;
    p.time_ms = 555;
    p.echo_ms = 444;
    p.has_sync = true;
    p.sync_tick = 77;
    p.sync_hash = 0xDEADBEEFCAFEBABEull;

    std::vector<u8> bytes;
    net::encode_packet(p, bytes);
    net::Packet q;
    CHECK(net::decode_packet(bytes, q));
    CHECK(q.player == 1 && q.start_tick == 1234 && q.frames.size() == 5);
    CHECK(q.frames[3] == p.frames[3]);
    CHECK(q.ack == 99 && q.time_ms == 555 && q.echo_ms == 444);
    CHECK(q.has_sync && q.sync_tick == 77 && q.sync_hash == p.sync_hash);

    bytes[0] ^= 0xFF; // bad magic
    CHECK(!net::decode_packet(bytes, q));
    bytes.clear();
    CHECK(!net::decode_packet(bytes, q));
}

void test_rollback_converges() {
    net::LoopbackPair link;
    link.set_conditions(50, 0, 0.0f); // ~3 ticks one way, no loss
    net::SessionConfig ca, cb;
    ca.local_player = 0;
    cb.local_player = 1;
    Peer a(ca, link.a());
    Peer b(cb, link.b());

    const Tick confirmed = run_pair(a, b, link, 400);
    CHECK(confirmed > 300);
    CHECK(a.session.stats().connected && b.session.stats().connected);
    CHECK(a.session.stats().rollbacks > 0); // predictions were wrong at times
    CHECK(states_agree(a, b, confirmed));
    CHECK(!a.session.stats().desync && !b.session.stats().desync);
    CHECK(a.session.stats().rtt_ms > 80 && a.session.stats().rtt_ms < 140);
    CHECK(a.timeline.head() > 350 && b.timeline.head() > 350);

    // Both games end up in the same place once both are past the confirmed tick.
    a.timeline.seek(a.game, confirmed);
    b.timeline.seek(b.game, confirmed);
    CHECK(a.game.pos[0] == b.game.pos[0] && a.game.pos[1] == b.game.pos[1] && a.game.mix == b.game.mix);
}

void test_loss_and_jitter() {
    net::LoopbackPair link;
    link.set_conditions(40, 60, 0.3f, 7); // ugly: 30% loss, up to 60 ms jitter
    net::SessionConfig ca, cb;
    ca.local_player = 0;
    cb.local_player = 1;
    ca.max_rollback = cb.max_rollback = 12;
    Peer a(ca, link.a());
    Peer b(cb, link.b());

    const Tick confirmed = run_pair(a, b, link, 600);
    CHECK(link.dropped() > 0);
    CHECK(confirmed > 400);
    CHECK(states_agree(a, b, confirmed));
    CHECK(!a.session.stats().desync && !b.session.stats().desync);
}

void test_stall_when_peer_is_silent() {
    net::LoopbackPair link;
    net::SessionConfig ca, cb;
    ca.local_player = 0;
    cb.local_player = 1;
    ca.max_rollback = 6;
    Peer a(ca, link.a());
    Peer b(cb, link.b());

    run_pair(a, b, link, 60, true); // b never steps, so a never hears from it
    CHECK(a.timeline.head() == ca.max_rollback);
    CHECK(a.session.stats().stalled_frames > 0);
    CHECK(!a.session.stats().connected);

    // Once the peer shows up, a catches up and they agree.
    const Tick confirmed = run_pair(a, b, link, 200);
    CHECK(a.timeline.head() > 100);
    CHECK(states_agree(a, b, confirmed));
}

void test_desync_is_detected() {
    net::LoopbackPair link;
    link.set_conditions(30, 0, 0.0f);
    net::SessionConfig ca, cb;
    ca.local_player = 0;
    cb.local_player = 1;
    ca.sync_interval = cb.sync_interval = 10;
    Peer a(ca, link.a());
    Peer b(cb, link.b());
    b.game.hidden_bug = true; // b's simulation silently differs after tick 40

    run_pair(a, b, link, 300);
    CHECK(a.session.stats().desync || b.session.stats().desync);
    const Tick reported = a.session.stats().desync ? a.session.stats().desync_tick : b.session.stats().desync_tick;
    CHECK(reported > 40);
}

void test_timeline_rollback_matches_fresh_run() {
    // Simulate, change one past input, roll back, and compare with a run that
    // had the corrected input from the start.
    Arena g1, g2;
    Timeline t1, t2;
    t1.reset(g1);
    t2.reset(g2);
    InputSet inputs[30];
    for (int i = 0; i < 30; ++i) {
        inputs[i].count = 2;
        inputs[i].players[0] = input_for(0, static_cast<Tick>(i));
        inputs[i].players[1] = input_for(1, static_cast<Tick>(i));
    }
    for (int i = 0; i < 30; ++i) t1.advance(g1, inputs[i]);

    InputSet corrected = inputs[12];
    corrected.players[1].axis_x = 31000;
    t1.set_inputs(12, corrected);
    const u32 rerun = t1.rollback(g1, 12);
    CHECK(rerun == 18);
    CHECK(t1.head() == 30);

    for (int i = 0; i < 30; ++i) t2.advance(g2, i == 12 ? corrected : inputs[i]);
    CHECK(g1.pos[1] == g2.pos[1] && g1.mix == g2.mix);
    for (Tick t = 0; t <= 30; ++t) CHECK(t1.hash_at(t) == t2.hash_at(t));

    // Rewritten history is consistent: a verifying replay finds no desync.
    t1.seek(g1, 0);
    t1.resimulate(g1, 30);
    CHECK(!t1.last_desync().has_value());
}

} // namespace

/// Four players pushing numbers around, like Arena but for any count.
struct Melee final : Game {
    i64 pos[kMaxPlayers] = {};
    u64 mix = 1;
    void tick(const TickContext& c) override {
        for (u32 p = 0; p < c.inputs.count; ++p) {
            const InputFrame& in = c.player(p);
            pos[p] += in.axis_x + (in.down(Button::A) ? 1000 : 0);
            mix = mix * 6364136223846793005ull + static_cast<u64>(in.buttons) + p;
        }
    }
    void reflect(Visitor& v) override {
        for (u32 p = 0; p < kMaxPlayers; ++p) v.field("p", pos[p]);
        v.field("mix", mix);
    }
    void render(Renderer2D&, float) override {}
};

void test_three_players_over_mesh() {
    net::LoopbackMesh mesh(3);
    mesh.set_conditions(40, 30, 0.15f, 3);
    for (u32 i = 0; i < 3; ++i)
        for (u32 j = 0; j < 3; ++j)
            if (i != j) mesh.end(i).add_peer(net::LoopbackMesh::address_of(j));

    TimelineConfig tcfg;
    tcfg.tick_rate = 60;
    tcfg.history_ticks = 600;
    tcfg.keyframe_interval = 1;
    tcfg.verify_hashes = true;
    Melee games[3];
    std::vector<std::unique_ptr<Timeline>> timelines;
    std::vector<std::unique_ptr<net::Session>> sessions;
    for (u32 i = 0; i < 3; ++i) {
        net::SessionConfig cfg;
        cfg.player_count = 3;
        cfg.local_player = i;
        cfg.input_delay = 3;
        timelines.push_back(std::make_unique<Timeline>(tcfg));
        timelines.back()->reset(games[i]);
        sessions.push_back(std::make_unique<net::Session>(cfg, mesh.end(i)));
    }
    for (int frame = 0; frame < 400; ++frame) {
        for (u32 i = 0; i < 3; ++i) {
            const Tick t = timelines[i]->head();
            sessions[i]->step(*timelines[i], games[i], input_for(i, t + 3), 1, nullptr, mesh.now_ms());
        }
        mesh.advance(16);
    }
    // Let everyone catch up on confirmed input, then compare confirmed states.
    for (int frame = 0; frame < 60; ++frame) {
        for (u32 i = 0; i < 3; ++i) sessions[i]->step(*timelines[i], games[i], InputFrame{}, 0, nullptr, mesh.now_ms());
        mesh.advance(16);
    }
    // Confirmed input can run ahead of a slow peer's head: compare a tick everyone has simulated.
    Tick confirmed = std::numeric_limits<Tick>::max();
    for (u32 i = 0; i < 3; ++i) confirmed = std::min({confirmed, sessions[i]->confirmed_tick(), timelines[i]->head()});
    CHECK(confirmed > 200);
    const Tick t = confirmed - 1;
    CHECK(timelines[0]->hash_at(t) == timelines[1]->hash_at(t));
    CHECK(timelines[1]->hash_at(t) == timelines[2]->hash_at(t));
    for (u32 i = 0; i < 3; ++i) {
        CHECK(!sessions[i]->stats().desync);
        CHECK(sessions[i]->stats().connected_players == 2);
        CHECK(sessions[i]->stats().rollbacks > 0);
    }
}

void test_lobby() {
    net::LoopbackMesh mesh(3);
    mesh.set_conditions(20, 10, 0.2f, 5);
    net::Lobby host(mesh.end(0)), a(mesh.end(1)), b(mesh.end(2));
    host.host("Host", 3);
    CHECK(host.state() == net::Lobby::State::Waiting && host.is_host() && host.local_slot() == 0);
    a.join(net::LoopbackMesh::address_of(0), "Alice");
    b.join(net::LoopbackMesh::address_of(0), "Bob");
    auto pump = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            host.update(mesh.now_ms());
            a.update(mesh.now_ms());
            b.update(mesh.now_ms());
            mesh.advance(16);
        }
    };
    pump(120);
    CHECK(a.state() == net::Lobby::State::Waiting && b.state() == net::Lobby::State::Waiting);
    CHECK(host.player_count() == 3);
    CHECK(a.local_slot() != b.local_slot() && a.local_slot() > 0 && b.local_slot() > 0);
    CHECK(a.players().size() == 3 && a.players()[0].name == "Host" && a.players()[a.local_slot()].name == "Alice");
    CHECK(a.players()[b.local_slot()].name == "Bob");
    CHECK(!host.all_ready());
    CHECK(!host.start(mesh.now_ms())); // not everyone is ready

    host.set_ready(true);
    a.set_ready(true);
    b.set_ready(true);
    pump(60);
    CHECK(host.all_ready() && a.all_ready());
    CHECK(host.start(mesh.now_ms(), 500, 77));
    CHECK(host.state() == net::Lobby::State::Starting);
    pump(20);
    CHECK(a.state() == net::Lobby::State::Starting && a.countdown_ms(mesh.now_ms()) > 0);
    pump(60);
    CHECK(host.state() == net::Lobby::State::Started && a.state() == net::Lobby::State::Started &&
          b.state() == net::Lobby::State::Started);
    CHECK(a.seed() == 77 && b.seed() == 77 && host.seed() == 77);

    // Everyone ends up with the same roster and their own peers.
    const net::SessionConfig ca = a.session_config(2);
    CHECK(ca.player_count == 3 && ca.local_player == a.local_slot());
    a.connect_peers(mesh.end(1));
    CHECK(mesh.end(1).peers().size() == 2);
    b.connect_peers(mesh.end(2));
    host.connect_peers(mesh.end(0));
    CHECK(mesh.end(0).peers().size() == 2);
    // The host's slot carries the address the clients joined through.
    CHECK(a.players()[0].address == net::LoopbackMesh::address_of(0));

    // A join to nobody fails after the timeout instead of hanging.
    net::LoopbackMesh alone(1);
    net::Lobby lonely(alone.end(0));
    lonely.join(net::Address{"mesh", 99}, "Nobody");
    for (int i = 0; i < 1000 && lonely.state() != net::Lobby::State::Failed; ++i) {
        lonely.update(alone.now_ms());
        alone.advance(16);
    }
    CHECK(lonely.state() == net::Lobby::State::Failed && !lonely.error().empty());
}

int main() {
    test_lobby();
    test_three_players_over_mesh();
    test_packet_round_trip();
    test_timeline_rollback_matches_fresh_run();
    test_rollback_converges();
    test_loss_and_jitter();
    test_stall_when_peer_is_silent();
    test_desync_is_detected();

    if (failures == 0) {
        std::printf("net_test: all checks passed\n");
        return 0;
    }
    std::printf("net_test: %d check(s) failed\n", failures);
    return 1;
}
