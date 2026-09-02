// Headless checks for rollback netcode: the wire format, two sessions
// talking over a lossy, laggy loopback, stalls, and desync detection.

#include <rewind/net/session.h>
#include <rewind/net/transport.h>
#include <rewind/sim/game.h>
#include <rewind/sim/serialize.h>
#include <rewind/sim/timeline.h>

#include <cstdio>
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

int main() {
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
