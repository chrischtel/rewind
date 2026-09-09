// Headless checks for the easing curves, approach() and Tweened.

#include <rewind/core/ease.h>
#include <rewind/sim/serialize.h>

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

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

const Ease kAll[] = {
    Ease::Linear,  Ease::InQuad,   Ease::OutQuad,   Ease::InOutQuad, Ease::InCubic,
    Ease::OutCubic, Ease::InOutCubic, Ease::InSine, Ease::OutSine,   Ease::InOutSine,
    Ease::InExpo,  Ease::OutExpo,  Ease::InOutExpo, Ease::OutBack,
};

void test_curves() {
    // Every curve starts at 0, ends at 1, and clamps outside [0, 1].
    for (const Ease k : kAll) {
        CHECK(near(ease(k, 0), 0));
        CHECK(near(ease(k, 1), 1));
        CHECK(near(ease(k, -5), 0));
        CHECK(near(ease(k, 5), 1));
    }

    // Out is ahead of the line early, In is behind it, Linear is the line.
    CHECK(ease(Ease::OutCubic, 0.25f) > 0.25f);
    CHECK(ease(Ease::OutQuad, 0.25f) > 0.25f);
    CHECK(ease(Ease::OutSine, 0.25f) > 0.25f);
    CHECK(ease(Ease::InCubic, 0.25f) < 0.25f);
    CHECK(ease(Ease::InQuad, 0.25f) < 0.25f);
    CHECK(near(ease(Ease::Linear, 0.25f), 0.25f));

    // The house curve is 1 - (1 - t)^3.
    CHECK(near(ease(Ease::OutCubic, 0.5f), 0.875f));
    CHECK(near(ease(Ease::InCubic, 0.5f), 0.125f));

    // Every InOut passes through the middle at the middle.
    CHECK(near(ease(Ease::InOutQuad, 0.5f), 0.5f));
    CHECK(near(ease(Ease::InOutCubic, 0.5f), 0.5f));
    CHECK(near(ease(Ease::InOutSine, 0.5f), 0.5f));
    CHECK(near(ease(Ease::InOutExpo, 0.5f), 0.5f));

    // Back is the only one allowed past 1, and it comes back.
    bool overshot = false;
    for (int i = 1; i < 100; ++i) {
        if (ease(Ease::OutBack, static_cast<float>(i) / 100.0f) > 1.0f) overshot = true;
    }
    CHECK(overshot);

    // Everything else rises the whole way, so a value never doubles back.
    for (const Ease k : kAll) {
        if (k == Ease::OutBack) continue;
        float previous = -1;
        for (int i = 0; i <= 100; ++i) {
            const float e = ease(k, static_cast<float>(i) / 100.0f);
            CHECK(e >= previous - 1e-5f);
            CHECK(e >= -1e-5f && e <= 1.0f + 1e-5f);
            previous = e;
        }
    }
}

void test_approach() {
    // Converges on the target from either side without passing it.
    float up = 0;
    float down = 20;
    for (int i = 0; i < 200; ++i) {
        up = approach(up, 10.0f, 8.0f, 1.0f / 60.0f);
        down = approach(down, 10.0f, 8.0f, 1.0f / 60.0f);
        CHECK(up <= 10.0f && down >= 10.0f);
    }
    CHECK(near(up, 10.0f, 1e-3f) && near(down, 10.0f, 1e-3f));

    // No step and no rate both mean no movement.
    CHECK(near(approach(3.0f, 10.0f, 8.0f, 0.0f), 3.0f));
    CHECK(near(approach(3.0f, 10.0f, 0.0f, 0.5f), 3.0f));

    // A faster rate gets further in the same step.
    CHECK(approach(0.0f, 10.0f, 16.0f, 1.0f / 60.0f) > approach(0.0f, 10.0f, 4.0f, 1.0f / 60.0f));

    // Vec2 moves on both axes.
    Vec2 p{0, 0};
    for (int i = 0; i < 200; ++i) p = approach(p, Vec2{4, -2}, 8.0f, 1.0f / 60.0f);
    CHECK(near(p.x, 4.0f, 1e-3f) && near(p.y, -2.0f, 1e-3f));
}

void test_tweened() {
    Tweened<float> t;
    t.duration = 0.2f;
    t.snap(5);
    CHECK(near(t.value, 5) && !t.moving() && near(t.progress(), 1.0f));

    // Nothing moves until it is ticked.
    t.set(10);
    CHECK(t.moving() && near(t.value, 5));

    t.tick(0.1f);
    const float half = t.value;
    CHECK(half > 5 && half < 10);

    // Setting the target it already has is a no-op, so tick() may call it
    // unconditionally every tick without restarting the move.
    t.set(10);
    CHECK(near(t.value, half) && near(t.elapsed, 0.1f));

    // It lands exactly on the target and stays there.
    t.tick(0.1f);
    CHECK(near(t.value, 10) && !t.moving());
    t.tick(0.1f);
    CHECK(near(t.value, 10) && near(t.elapsed, 0.2f));

    // Redirected mid-move it starts from where it is, not from where it was.
    Tweened<float> r;
    r.duration = 0.2f;
    r.snap(0);
    r.set(10);
    r.tick(0.1f);
    const float mid = r.value;
    r.set(0);
    CHECK(near(r.from, mid) && near(r.elapsed, 0) && near(r.target, 0));
    r.tick(0.2f);
    CHECK(near(r.value, 0));

    // A zero duration is an instant set, not a divide by zero.
    Tweened<float> instant;
    instant.duration = 0;
    instant.set(3);
    instant.tick(1.0f / 60.0f);
    CHECK(near(instant.value, 3) && !instant.moving());

    // Vec2 rides the same machinery.
    Tweened<Vec2> p;
    p.duration = 0.1f;
    p.snap({0, 0});
    p.set({10, -4});
    p.tick(0.1f);
    CHECK(near(p.value.x, 10) && near(p.value.y, -4));

    // A move in progress survives save and load, and finishes afterwards.
    Tweened<float> saved;
    saved.duration = 0.5f;
    saved.snap(1);
    saved.set(9);
    saved.tick(0.25f);
    Writer w;
    {
        SaveVisitor save(w);
        saved.reflect(save);
    }
    Tweened<float> loaded;
    {
        Reader rd(w.bytes());
        LoadVisitor load(rd);
        loaded.reflect(load);
    }
    CHECK(near(loaded.value, saved.value) && near(loaded.elapsed, 0.25f));
    CHECK(near(loaded.target, 9) && near(loaded.from, 1));
    loaded.tick(0.25f);
    CHECK(near(loaded.value, 9) && !loaded.moving());
}

} // namespace

int main() {
    test_curves();
    test_approach();
    test_tweened();
    if (failures == 0) std::printf("ease_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
