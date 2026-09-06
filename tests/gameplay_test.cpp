// Headless checks for steering, the path follower, timers, cooldowns and state machines.

#include <rewind/sim/serialize.h>
#include <rewind/sim/steering.h>
#include <rewind/sim/timer.h>

#include <cmath>
#include <cstdio>

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

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool near(Vec2 a, Vec2 b, float eps = 1e-3f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps); }

void test_steering() {
    CHECK(near(seek({0, 0}, {10, 0}, 5), {5, 0}));
    CHECK(near(seek({0, 0}, {0, 0}, 5), {0, 0}));
    CHECK(near(flee({0, 0}, {10, 0}, 5), {-5, 0}));
    CHECK(near(length(limit({30, 40}, 10)), 10));
    CHECK(near(limit({3, 4}, 10), {3, 4}));

    // arrive: full speed far away, proportional inside the slow radius, zero at the stop radius.
    CHECK(near(arrive({0, 0}, {100, 0}, 10, 20), {10, 0}));
    CHECK(near(arrive({0, 0}, {10, 0}, 10, 20), {5, 0}));
    CHECK(near(arrive({0, 0}, {1, 0}, 10, 20, 2), {0, 0}));

    // pursue leads the target.
    const Vec2 p = pursue({0, 0}, {10, 0}, {0, 10}, 10);
    CHECK(p.y > 0 && p.x > 0 && near(length(p), 10));

    // keep_distance: too far -> approach, too close -> back off, ok -> nothing.
    CHECK(keep_distance({20, 0}, {0, 0}, 10, 5).x < 0);
    CHECK(keep_distance({3, 0}, {0, 0}, 10, 5).x > 0);
    CHECK(near(keep_distance({10.5f, 0}, {0, 0}, 10, 5), {0, 0}));
    CHECK(near(length(keep_distance({100, 0}, {0, 0}, 10, 5)), 5)); // capped at max speed

    // separation pushes away from close neighbours only.
    const Vec2 s = separation({0, 0}, {{5, 0}, {0, -50}}, 10, 7);
    CHECK(s.x < 0 && near(s.y, 0));
    CHECK(near(separation({0, 0}, {{50, 0}}, 10, 7), {0, 0}));

    // turn_toward goes the short way and is capped.
    CHECK(near(turn_toward(0, 3.0f, 0.5f), 0.5f));
    CHECK(near(turn_toward(0, -3.0f, 0.5f), -0.5f));
    CHECK(near(turn_toward(0.1f, 6.2f, 1.0f), 0.1f - 0.1831853f, 1e-3f)); // 6.2 is just below 0 going around
    CHECK(near(turn_toward(1, 1.2f, 1), 1.2f));
}

void test_path_follower() {
    Path path = Path::from_polyline({{0, 0}, {100, 0}, {100, 100}});
    PathSampler sampler(path);
    PathFollower f;
    f.speed = 50;
    f.lookahead = 10;
    Vec2 pos{0, 0};
    int ticks = 0;
    while (!f.done && ticks < 600) {
        const Vec2 v = f.update(sampler, pos, 1.0f / 60);
        pos += v * (1.0f / 60);
        ++ticks;
    }
    CHECK(f.done);
    CHECK(near(pos, {100, 100}, 6));
    CHECK(ticks > 200 && ticks < 300); // 200 units at 50/s = 4 s = 240 ticks, plus arrival

    // Lagging agent: the target waits, distance does not run away.
    PathFollower lag;
    lag.speed = 50;
    lag.lookahead = 10;
    lag.update(sampler, {0, 0}, 0.1f);
    const float d1 = lag.distance;
    lag.update(sampler, {0, 0}, 0.1f); // still at the start, far behind d1 + lookahead? no: 5 < 10, so it advances once more
    lag.update(sampler, {0, 0}, 0.1f);
    lag.update(sampler, {0, 0}, 0.1f);
    CHECK(lag.distance < d1 + 20); // stalls once the lag exceeds lookahead

    // Closed path wraps and never finishes.
    Path loop;
    loop.move_to({50, 0});
    loop.arc_to({0, 0}, 6.2831853f);
    loop.close();
    PathSampler ls(loop);
    PathFollower lf;
    lf.speed = 100;
    lf.lookahead = 8;
    Vec2 lp{50, 0};
    for (int i = 0; i < 600; ++i) {
        lp += lf.update(ls, lp, 1.0f / 60) * (1.0f / 60);
    }
    CHECK(!lf.done);
    CHECK(near(length(lp), 50, 3)); // stays on the circle
    CHECK(lf.distance < ls.length() * 2 + 1);
}

void test_timers() {
    Timer t;
    t.start(1.0f);
    CHECK(t.running && !t.tick(0.5f) && near(t.progress(), 0.5f) && near(t.remaining(), 0.5f));
    CHECK(t.tick(0.6f));
    CHECK(!t.running && t.finished() && near(t.progress(), 1));
    CHECK(!t.tick(1)); // stopped timers stay quiet

    Timer loop;
    loop.start(0.25f, true);
    int fired = 0;
    for (int i = 0; i < 60; ++i)
        if (loop.tick(1.0f / 60)) ++fired;
    CHECK(fired == 4);
    CHECK(loop.running);

    Cooldown c;
    CHECK(c.ready());
    c.use(0.5f);
    CHECK(!c.ready() && !c.tick(0.2f));
    CHECK(c.tick(0.4f) && c.ready() && c.remaining == 0);
    CHECK(!c.tick(1));
}

enum class Mood : u8 { Idle, Chase, Flee };
const char* const kMoodNames[] = {"idle", "chase", "flee"};

void test_state_machine() {
    StateMachine<Mood> fsm(Mood::Idle, kMoodNames, 3);
    CHECK(fsm.is(Mood::Idle) && fsm.entered);
    fsm.tick(0.1f);
    CHECK(!fsm.entered && near(fsm.time_in_state, 0.1f));
    CHECK(!fsm.change(Mood::Idle) && fsm.transitions == 0);
    CHECK(fsm.change(Mood::Chase) && fsm.entered && fsm.was(Mood::Idle) && fsm.transitions == 1);
    CHECK(near(fsm.time_in_state, 0));
    CHECK(std::string(fsm.name(fsm.current)) == "chase");
    fsm.tick(0.5f);
    fsm.restart();
    CHECK(fsm.entered && fsm.transitions == 2 && fsm.is(Mood::Chase));

    // Round trip through save/load keeps the state (names are not data).
    Writer w;
    {
        SaveVisitor save(w);
        fsm.reflect(save);
    }
    StateMachine<Mood> loaded;
    {
        Reader r(w.bytes());
        LoadVisitor load(r);
        loaded.reflect(load);
    }
    CHECK(loaded.is(Mood::Chase) && loaded.was(Mood::Idle) && loaded.transitions == 2);
}

} // namespace

int main() {
    test_steering();
    test_path_follower();
    test_timers();
    test_state_machine();
    if (failures == 0) std::printf("gameplay_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
