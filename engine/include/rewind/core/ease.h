#pragma once

// Easing and tweening as plain reflected data: a value that is halfway
// through moving lives in your game state, so it rewinds, replays, saves and
// shows in the inspector like everything else. Drive it from tick() with the
// fixed dt and read it in render().
//
// The rule a game that wants to feel finished follows: nothing the eye can
// follow ever jumps. Put the value in a Tweened, set() the target whenever it
// changes, and the motion is right by default instead of by discipline.

#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/sim/reflect.h"

#include <cmath>

namespace rw {

/// The shape a move takes over its duration. Out is the everyday one: quick
/// off the mark and settling at the end, which reads as answered rather than
/// animated. Reach for another only when Out is visibly wrong.
enum class Ease : u8 {
    Linear,
    InQuad,
    OutQuad,
    InOutQuad,
    InCubic,
    OutCubic,
    InOutCubic,
    InSine,
    OutSine,
    InOutSine,
    InExpo,
    OutExpo,
    InOutExpo,
    OutBack,
};

/// For `FieldMeta::names`, so a curve field is a dropdown in the inspector.
inline const char* const kEaseNames[] = {
    "Linear", "InQuad", "OutQuad", "InOutQuad", "InCubic", "OutCubic", "InOutCubic",
    "InSine", "OutSine", "InOutSine", "InExpo", "OutExpo", "InOutExpo", "OutBack",
};
inline constexpr u32 kEaseNameCount = sizeof(kEaseNames) / sizeof(kEaseNames[0]);

/// Fraction of the way there, for a fraction of the way through. `t` is
/// clamped, so an overshooting caller is safe.
inline float ease(Ease kind, float t) {
    t = clampf(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    switch (kind) {
    case Ease::Linear: return t;
    case Ease::InQuad: return t * t;
    case Ease::OutQuad: return 1.0f - u * u;
    case Ease::InOutQuad: return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * u * u;
    case Ease::InCubic: return t * t * t;
    case Ease::OutCubic: return 1.0f - u * u * u;
    case Ease::InOutCubic: return t < 0.5f ? 4.0f * t * t * t : 1.0f - 4.0f * u * u * u;
    case Ease::InSine: return 1.0f - std::cos(t * 1.57079633f);
    case Ease::OutSine: return std::sin(t * 1.57079633f);
    case Ease::InOutSine: return 0.5f * (1.0f - std::cos(t * 3.14159265f));
    case Ease::InExpo: return t <= 0.0f ? 0.0f : std::pow(2.0f, 10.0f * t - 10.0f);
    case Ease::OutExpo: return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
    case Ease::InOutExpo:
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        return t < 0.5f ? 0.5f * std::pow(2.0f, 20.0f * t - 10.0f)
                        : 1.0f - 0.5f * std::pow(2.0f, -20.0f * t + 10.0f);
    /// Overshoots and comes back. Only for something arriving, never for a
    /// number the player is reading.
    case Ease::OutBack: return 1.0f - 2.70158f * (u * u * u) + 1.70158f * (u * u);
    }
    return t;
}

/// Move `current` a proportion of the way to `target` every second, which is
/// the shape a camera or a follow wants: no duration, no end, and the same
/// result for the same dt. `rate` is roughly "e-folds per second"; 8 is a
/// brisk follow, 2 a lazy one.
inline float approach(float current, float target, float rate, float dt) {
    const float k = 1.0f - std::exp(-rate * dt);
    return current + (target - current) * k;
}
inline Vec2 approach(Vec2 current, Vec2 target, float rate, float dt) {
    const float k = 1.0f - std::exp(-rate * dt);
    return current + (target - current) * k;
}

/// How Tweened blends. Found by ordinary lookup for the engine's own types
/// and by argument-dependent lookup for a game's, so a game tweens its own
/// type by writing one overload next to it:
///
///     inline Money tween_lerp(Money a, Money b, float t) { return {lerpf(a.v, b.v, t)}; }
inline float tween_lerp(float a, float b, float t) { return a + (b - a) * t; }
inline Vec2 tween_lerp(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }

/// A value that eases toward whatever it is last told to be.
///
///     Tweened<float> money;              // in the game state, reflected
///     money.snap(20000);                 // in init(), no animation
///     money.set(balance);                // in tick(), every tick, harmless
///     money.tick(ctx.dt);
///     draw_number(money.value);          // in render()
///
/// set() with the target it already has does nothing, so calling it
/// unconditionally every tick is the intended use. `T` needs `operator==`
/// and a `tween_lerp`.
template <class T> struct Tweened {
    T value{};  ///< What to draw. Everything else is bookkeeping.
    T from{};
    T target{};
    float elapsed = 0;
    float duration = 0.18f;             ///< 180 ms: a panel, a tab, a hover.
    Ease curve = Ease::OutCubic;

    /// Be this value now, with no move. For init() and for anything the
    /// player did not cause, like loading a save.
    void snap(const T& v) {
        value = from = target = v;
        elapsed = duration;
    }
    /// Move to this value over `duration`. A no-op when already heading there.
    void set(const T& v) {
        if (v == target) return;
        from = value;
        target = v;
        elapsed = 0;
    }
    /// Same, with a duration for this move onwards.
    void set(const T& v, float seconds) {
        duration = seconds;
        set(v);
    }
    /// True while there is still a move to draw. A snapped value is not
    /// moving even if `duration` was changed under it, because `from` and
    /// `target` agree.
    bool moving() const { return elapsed < duration && !(from == target); }
    float progress() const { return duration > 0 ? clampf(elapsed / duration, 0.0f, 1.0f) : 1.0f; }

    void tick(float dt) {
        if (duration <= 0) {
            value = target;
            elapsed = 0;
            return;
        }
        if (elapsed >= duration) {
            value = target;
            return;
        }
        elapsed += dt;
        if (elapsed >= duration) {
            elapsed = duration;
            value = target;
            return;
        }
        value = tween_lerp(from, target, ease(curve, elapsed / duration));
    }

    void reflect(Visitor& v) {
        v.field("value", value, {.read_only = true});
        v.field("from", from);
        v.field("target", target);
        v.field("elapsed", elapsed, {.min = 0});
        v.field("duration", duration, {.min = 0});
        v.field("curve", curve, {.names = kEaseNames, .name_count = kEaseNameCount});
    }
};

} // namespace rw
