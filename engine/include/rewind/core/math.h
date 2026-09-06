#pragma once

#include "rewind/sim/reflect.h"

namespace rw {

struct Vec2 {
    float x = 0;
    float y = 0;

    void reflect(Visitor& v) {
        v.hint(Widget::Vec2);
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
        v.hint(Widget::Rect);
        v.field("x", x);
        v.field("y", y);
        v.field("w", w);
        v.field("h", h);
    }

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

struct Vec3 {
    float x = 0;
    float y = 0;
    float z = 0;

    void reflect(Visitor& v) {
        v.field("x", x);
        v.field("y", y);
        v.field("z", z);
    }
};

/// 4x4 matrix, column-major in memory (m[col * 4 + row]), which is what
/// the shaders read. Rendering uses it for the view-projection and for
/// optional per-mesh transforms; 2D games never have to touch it.
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static Mat4 identity() { return Mat4{}; }
    static Mat4 translation(float x, float y, float z = 0);
    static Mat4 scaling(float x, float y, float z = 1);
    static Mat4 rotation_z(float radians);
    /// Pixel space to clip space: (0,0) top-left, (w,h) bottom-right, z in [-1, 1] maps to depth.
    static Mat4 ortho_pixels(float width, float height);
    static Mat4 ortho(float left, float right, float bottom, float top, float near, float far);
    static Mat4 perspective(float fov_y_radians, float aspect, float near, float far);
    static Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up);

    Mat4 operator*(const Mat4& rhs) const;
    Vec3 transform_point(Vec3 p) const;
    Vec3 transform_direction(Vec3 d) const;
    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }
};

// ---- Vec2 arithmetic ------------------------------------------------------------------

inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator-(Vec2 a) { return {-a.x, -a.y}; }
inline Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
inline Vec2 operator*(float s, Vec2 a) { return {a.x * s, a.y * s}; }
inline Vec2 operator/(Vec2 a, float s) { return {a.x / s, a.y / s}; }
inline Vec2& operator+=(Vec2& a, Vec2 b) { a = a + b; return a; }
inline Vec2& operator-=(Vec2& a, Vec2 b) { a = a - b; return a; }
inline Vec2& operator*=(Vec2& a, float s) { a = a * s; return a; }
inline bool operator==(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(Vec2 a, Vec2 b) { return !(a == b); }

inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
/// 2D cross product (z of the 3D cross): positive when b is counter-clockwise from a in y-up space.
inline float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline float length_sq(Vec2 a) { return a.x * a.x + a.y * a.y; }
float length(Vec2 a);
float distance(Vec2 a, Vec2 b);
/// Unit vector, or (1, 0) for a zero vector.
Vec2 normalize(Vec2 a);
/// Rotated 90 degrees: (-y, x). In y-down screen space that is the right-hand side of travel.
inline Vec2 perp(Vec2 a) { return {-a.y, a.x}; }
inline Vec2 lerp(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }
Vec2 rotate(Vec2 v, float radians);
Vec2 from_angle(float radians);
float angle_of(Vec2 v);

inline bool overlaps(const Rect& a, const Rect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float absf(float v) { return v < 0 ? -v : v; }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

} // namespace rw
