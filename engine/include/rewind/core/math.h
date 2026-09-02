#pragma once

#include "rewind/sim/reflect.h"

namespace rw {

struct Vec2 {
    float x = 0;
    float y = 0;

    void reflect(Visitor& v) {
        v.field("x", x);
        v.field("y", y);
    }
};

/// Axis-aligned rectangle: top-left corner plus size.
struct Rect {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;

    void reflect(Visitor& v) {
        v.field("x", x);
        v.field("y", y);
        v.field("w", w);
        v.field("h", h);
    }

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

inline bool overlaps(const Rect& a, const Rect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float absf(float v) { return v < 0 ? -v : v; }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

} // namespace rw
