// Headless checks for the timeline: rewind, branching, trimming, keyframes,
// and the desync detector. No window, no SDL. Run via ctest or directly.

#include <rewind/sim/rng.h>
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

/// A tiny "game" whose state depends on every input and on its own RNG.
struct Counter final : Game {
    u64 value = 0;
    Rng rng{7};
    bool hidden_bug = false; ///< Behaviour change that is NOT saved, to provoke a desync.

    void tick(const TickContext& c) override {
        value = value * 31 + c.input.buttons + c.input.axis_x + rng.next_u32();
        if (hidden_bug) value += 1;
        if (c.tick % 10 == 0) c.emit(7, static_cast<float>(c.tick)); // every tenth tick says hello
    }
    void save(Writer& w) const override {
        w.pod(value);
        w.pod(rng);
    }
    void load(Reader& r) override {
        r.pod(value);
        r.pod(rng);
    }
    void render(Renderer2D&, float) override {}
};

InputFrame input_for(Tick t) {
    InputFrame in;
    in.buttons = static_cast<u32>(t % 7);
    in.axis_x = static_cast<i16>((t * 13) % 100);
    return in;
}

/// Advance `ticks` steps and return value-at-tick for every tick 0..ticks.
std::vector<u64> drive(Timeline& tl, Counter& game, Tick ticks) {
    std::vector<u64> values{game.value};
    for (Tick t = 0; t < ticks; ++t) {
        tl.advance(game, input_for(t));
        values.push_back(game.value);
    }
    return values;
}

void test_seek_restores_exact_state() {
    Counter game;
    Timeline tl;
    tl.reset(game);
    const auto values = drive(tl, game, 100);

    CHECK(tl.head() == 100);
    CHECK(tl.newest() == 100);
    CHECK(tl.oldest() == 0);

    tl.seek(game, 50);
    CHECK(tl.head() == 50);
    CHECK(game.value == values[50]);

    tl.seek(game, 0);
    CHECK(game.value == values[0]);

    tl.seek(game, 100);
    CHECK(game.value == values[100]);
    CHECK(!tl.last_desync().has_value());

    // Walking backwards one tick at a time, like holding the rewind key.
    for (Tick t = 100; t-- > 0;) {
        tl.seek(game, t);
        CHECK(game.value == values[t]);
    }
    CHECK(!tl.last_desync().has_value());
}

void test_resuming_from_the_past_branches() {
    Counter game;
    Timeline tl;
    tl.reset(game);
    const auto values = drive(tl, game, 100);

    tl.seek(game, 40);
    InputFrame different;
    different.buttons = 0xFFFF;
    tl.advance(game, different);

    CHECK(tl.head() == 41);
    CHECK(tl.newest() == 41);
    CHECK(game.value != values[41]);
    CHECK(tl.input_at(40) == different);
}

void test_step_forward_replays_then_advances() {
    Counter game;
    Timeline tl;
    tl.reset(game);
    const auto values = drive(tl, game, 10);

    tl.seek(game, 5);
    tl.step_forward(game, InputFrame{});
    CHECK(tl.head() == 6);
    CHECK(game.value == values[6]); // recorded future replayed, live input ignored

    tl.seek(game, 10);
    tl.step_forward(game, InputFrame{});
    CHECK(tl.head() == 11);
    CHECK(tl.newest() == 11);
}

void test_history_is_trimmed() {
    Counter game;
    TimelineConfig cfg;
    cfg.history_ticks = 20;
    Timeline tl(cfg);
    tl.reset(game);
    const auto values = drive(tl, game, 100);

    CHECK(tl.newest() == 100);
    CHECK(tl.oldest() == 80);
    CHECK(tl.memory_bytes() > 0);

    tl.seek(game, 80);
    CHECK(game.value == values[80]);
    tl.seek(game, 0); // clamped to the oldest retained tick
    CHECK(tl.head() == 80);
}

void test_sparse_keyframes() {
    Counter game;
    TimelineConfig cfg;
    cfg.keyframe_interval = 10;
    Timeline tl(cfg);
    tl.reset(game);
    const auto values = drive(tl, game, 100);

    CHECK(tl.keyframe_count() == 11);

    tl.seek(game, 55); // loads keyframe 50 and re-simulates 5 ticks
    CHECK(game.value == values[55]);
    tl.seek(game, 99);
    CHECK(game.value == values[99]);
    CHECK(!tl.last_desync().has_value());

    // Trimming with sparse keyframes must keep a keyframe at the front.
    TimelineConfig small;
    small.keyframe_interval = 10;
    small.history_ticks = 25;
    Timeline tl2(small);
    Counter g2;
    tl2.reset(g2);
    const auto v2 = drive(tl2, g2, 100);
    CHECK(tl2.oldest() % 10 == 0);
    CHECK(tl2.newest() - tl2.oldest() <= 35);
    tl2.seek(g2, tl2.oldest() + 3);
    CHECK(g2.value == v2[static_cast<usize>(tl2.oldest() + 3)]);
}

void test_resimulate_verifies_and_matches() {
    Counter game;
    Timeline tl;
    tl.reset(game);
    const auto values = drive(tl, game, 30);

    tl.seek(game, 0);
    tl.resimulate(game, 30);
    CHECK(tl.head() == 30);
    CHECK(game.value == values[30]);
    CHECK(!tl.last_desync().has_value());

    tl.seek(game, 10);
    tl.resimulate(game, 5); // cannot go backwards: clamped to head
    CHECK(tl.head() == 10);
    tl.resimulate(game, 1000); // clamped to newest
    CHECK(tl.head() == 30);
    CHECK(game.value == values[30]);
}

void test_desync_is_detected() {
    Counter game;
    Timeline tl;
    tl.reset(game);
    drive(tl, game, 30);

    // With a snapshot on every tick, seek alone never runs the game, so it
    // cannot notice anything. Replay must re-simulate.
    game.hidden_bug = true; // simulation now behaves differently than recorded
    tl.seek(game, 0);
    tl.seek(game, 30);
    CHECK(!tl.last_desync().has_value());

    tl.seek(game, 0);
    tl.resimulate(game, 30);
    CHECK(tl.last_desync().has_value());
    if (tl.last_desync()) {
        CHECK(tl.last_desync()->tick == 1);
        CHECK(tl.last_desync()->expected != tl.last_desync()->actual);
    }
    tl.clear_desync();
    CHECK(!tl.last_desync().has_value());
}

void test_events_only_from_live_ticks() {
    Counter game;
    Timeline tl;
    tl.reset(game);

    EventQueue events;
    for (Tick t = 0; t < 30; ++t) tl.advance(game, input_for(t), &events);
    CHECK(events.items.size() == 3); // ticks 0, 10, 20
    CHECK(events.items[1].id == 7 && events.items[1].a == 10.0f);

    // Rewinding, replaying and stepping through recorded history stay silent.
    events.clear();
    tl.seek(game, 0);
    tl.resimulate(game, 30);
    tl.seek(game, 5);
    tl.step_forward(game, InputFrame{});
    CHECK(events.empty());

    // Advancing without a queue is fine too.
    tl.seek(game, 30);
    tl.advance(game, input_for(30));
    CHECK(tl.head() == 31);
}

void test_edits_become_part_of_history() {
    Counter game;
    Timeline tl;
    tl.reset(game);
    const auto values = drive(tl, game, 20);

    // Go back, change the state by hand, tell the timeline.
    tl.seek(game, 10);
    game.value = 424242;
    tl.recapture(game);
    CHECK(tl.newest() == 10); // the old future is gone
    CHECK(tl.head() == 10);

    // Continue from the edit.
    for (Tick t = 10; t < 20; ++t) tl.advance(game, input_for(t));
    const u64 edited_end = game.value;
    CHECK(edited_end != values[20]);

    // Replaying through the edit restores the edited state instead of
    // re-deriving the original one, and reports no desync.
    tl.seek(game, 0);
    tl.resimulate(game, 20);
    CHECK(game.value == edited_end);
    CHECK(!tl.last_desync().has_value());

    tl.seek(game, 10);
    CHECK(game.value == 424242);
}

void test_reader_never_reads_past_the_end() {
    Writer w;
    w.pod<u32>(42);
    Reader r(w.bytes());
    CHECK(r.pod<u32>() == 42);
    CHECK(r.ok());
    CHECK(r.pod<u64>() == 0);
    CHECK(!r.ok());

    std::vector<u32> big;
    Writer w2;
    w2.pod<u64>(1u << 30); // claims a huge vector with no data behind it
    Reader r2(w2.bytes());
    r2.vec(big);
    CHECK(big.empty());
    CHECK(!r2.ok());
}

} // namespace

int main() {
    test_seek_restores_exact_state();
    test_resuming_from_the_past_branches();
    test_step_forward_replays_then_advances();
    test_history_is_trimmed();
    test_sparse_keyframes();
    test_resimulate_verifies_and_matches();
    test_desync_is_detected();
    test_events_only_from_live_ticks();
    test_edits_become_part_of_history();
    test_reader_never_reads_past_the_end();

    if (failures == 0) {
        std::printf("timeline_test: all checks passed\n");
        return 0;
    }
    std::printf("timeline_test: %d check(s) failed\n", failures);
    return 1;
}
