#pragma once

// Steering: small pure functions that turn "where do I want to be" into a
// desired velocity, plus a path follower. Combine the results (add, then
// limit) and integrate them yourself, so movement stays under your tick's
// control and deterministic.

#include "rewind/core/curve.h"
#include "rewind/core/math.h"
#include "rewind/sim/reflect.h"

#include <cmath>
#include <vector>

namespace rw {

/// Clamp a vector's length.
inline Vec2 limit(Vec2 v, float max_length) {
    const float l2 = length_sq(v);
    if (max_length <= 0 || l2 <= max_length * max_length) return v;
    return v * (max_length / std::sqrt(l2));
}

/// Full speed towards `target`.
inline Vec2 seek(Vec2 pos, Vec2 target, float max_speed) {
    const Vec2 d = target - pos;
    return length_sq(d) > 1e-12f ? normalize(d) * max_speed : Vec2{};
}

inline Vec2 flee(Vec2 pos, Vec2 threat, float max_speed) { return -seek(pos, threat, max_speed); }

/// Towards `target`, slowing inside `slow_radius`, stopped inside `stop_radius`.
inline Vec2 arrive(Vec2 pos, Vec2 target, float max_speed, float slow_radius, float stop_radius = 0) {
    const Vec2 d = target - pos;
    const float dist = length(d);
    if (dist <= stop_radius || dist < 1e-6f) return {};
    float speed = max_speed;
    if (slow_radius > stop_radius && dist < slow_radius) speed = max_speed * (dist - stop_radius) / (slow_radius - stop_radius);
    return d * (speed / dist);
}

/// Seek where a moving target will be, assuming constant velocity.
inline Vec2 pursue(Vec2 pos, Vec2 target, Vec2 target_velocity, float max_speed) {
    const float dist = length(target - pos);
    const float t = max_speed > 0 ? dist / max_speed : 0;
    return seek(pos, target + target_velocity * t, max_speed);
}

/// Stay `desired` away from `other`: approach when farther, back off when
/// closer, nothing within `tolerance` of the desired distance.
inline Vec2 keep_distance(Vec2 pos, Vec2 other, float desired, float max_speed, float tolerance = 1) {
    const Vec2 d = pos - other;
    const float dist = length(d);
    if (dist < 1e-6f) return Vec2{max_speed, 0};
    const float error = dist - desired; // positive: too far
    if (absf(error) <= tolerance) return {};
    const Vec2 dir = d / dist;
    return -dir * (error > 0 ? std::min(max_speed, error) : -std::min(max_speed, -error));
}

/// Push away from every neighbour closer than `radius`, stronger the closer.
inline Vec2 separation(Vec2 pos, const std::vector<Vec2>& neighbours, float radius, float max_speed) {
    Vec2 sum{};
    for (const Vec2& n : neighbours) {
        const Vec2 d = pos - n;
        const float dist = length(d);
        if (dist >= radius || dist < 1e-6f) continue;
        sum += d / dist * ((radius - dist) / radius);
    }
    return limit(sum * max_speed, max_speed);
}

/// Rotate `current` towards `target` (radians) by at most `max_turn`, the short way round.
inline float turn_toward(float current, float target, float max_turn) {
    float delta = std::fmod(target - current, 6.2831853f);
    if (delta > 3.14159265f) delta -= 6.2831853f;
    if (delta < -3.14159265f) delta += 6.2831853f;
    return current + clampf(delta, -max_turn, max_turn);
}

/// Progress along a Path: advances a distance at up to `max_speed` and
/// seeks a point `lookahead` ahead of it, which keeps agents on the curve
/// without ever losing their place. Reflects; pair it with a PathSampler
/// built from the same path (the sampler is derived data, rebuild it on load).
struct PathFollower {
    float distance = 0;
    float lookahead = 24;
    float speed = 100;
    bool done = false;

    void reset(float at = 0) {
        distance = at;
        done = false;
    }

    /// Desired velocity for this tick. Advances along the path by speed * dt
    /// unless the agent is lagging more than `lookahead` behind, in which
    /// case the target waits for it.
    Vec2 update(const PathSampler& path, Vec2 pos, float dt) {
        if (path.empty()) return {};
        const float total = path.length();
        const PathPoint here = path.at(distance);
        const float lag = rw::distance(pos, here.position);
        if (lag < lookahead) distance += speed * dt;
        if (path.closed()) {
            if (total > 0) distance = std::fmod(distance, total); // stay small: floats lose precision when they grow
        } else {
            if (distance >= total) {
                distance = total;
                const Vec2 to_end = path.at(total).position - pos;
                if (length(to_end) <= lookahead * 0.5f) done = true;
                return arrive(pos, path.at(total).position, speed, lookahead, 0.5f);
            }
        }
        const Vec2 target = path.at(distance + lookahead).position;
        return seek(pos, target, speed);
    }

    void reflect(Visitor& v) {
        v.field("distance", distance, {.min = 0});
        v.field("lookahead", lookahead, {.min = 0, .max = 1000});
        v.field("speed", speed, {.min = 0, .max = 10000});
        v.field("done", done);
    }
};

} // namespace rw
