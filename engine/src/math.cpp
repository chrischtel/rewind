#include "rewind/core/math.h"

#include <cmath>

namespace rw {

float length(Vec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
float distance(Vec2 a, Vec2 b) { return length(a - b); }
Vec2 normalize(Vec2 a) {
    const float l = length(a);
    return l > 1e-8f ? a / l : Vec2{1, 0};
}
Vec2 rotate(Vec2 v, float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}
Vec2 from_angle(float radians) { return {std::cos(radians), std::sin(radians)}; }
float angle_of(Vec2 v) { return std::atan2(v.y, v.x); }

Mat4 Mat4::translation(float x, float y, float z) {
    Mat4 r;
    r.at(0, 3) = x;
    r.at(1, 3) = y;
    r.at(2, 3) = z;
    return r;
}

Mat4 Mat4::scaling(float x, float y, float z) {
    Mat4 r;
    r.at(0, 0) = x;
    r.at(1, 1) = y;
    r.at(2, 2) = z;
    return r;
}

Mat4 Mat4::rotation_z(float radians) {
    Mat4 r;
    const float c = std::cos(radians), s = std::sin(radians);
    r.at(0, 0) = c;
    r.at(0, 1) = -s;
    r.at(1, 0) = s;
    r.at(1, 1) = c;
    return r;
}

Mat4 Mat4::ortho(float left, float right, float bottom, float top, float near, float far) {
    Mat4 r;
    r.at(0, 0) = 2.0f / (right - left);
    r.at(1, 1) = 2.0f / (top - bottom);
    r.at(2, 2) = -2.0f / (far - near);
    r.at(0, 3) = -(right + left) / (right - left);
    r.at(1, 3) = -(top + bottom) / (top - bottom);
    r.at(2, 3) = -(far + near) / (far - near);
    return r;
}

Mat4 Mat4::ortho_pixels(float width, float height) {
    // y down on screen: top is 0, bottom is height. Depth: z -1..1 -> near..far.
    return ortho(0, width, height, 0, -1.0f, 1.0f);
}

Mat4 Mat4::perspective(float fov_y, float aspect, float near, float far) {
    Mat4 r;
    const float f = 1.0f / std::tan(fov_y * 0.5f);
    r.at(0, 0) = f / aspect;
    r.at(1, 1) = f;
    r.at(2, 2) = (far + near) / (near - far);
    r.at(2, 3) = 2.0f * far * near / (near - far);
    r.at(3, 2) = -1.0f;
    r.at(3, 3) = 0.0f;
    return r;
}

Mat4 Mat4::look_at(Vec3 eye, Vec3 target, Vec3 up) {
    auto norm = [](Vec3 v) {
        const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return l > 0 ? Vec3{v.x / l, v.y / l, v.z / l} : v;
    };
    auto cross = [](Vec3 a, Vec3 b) { return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
    auto dot = [](Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const Vec3 f = norm(Vec3{target.x - eye.x, target.y - eye.y, target.z - eye.z});
    const Vec3 s = norm(cross(f, up));
    const Vec3 u = cross(s, f);
    Mat4 r;
    r.at(0, 0) = s.x; r.at(0, 1) = s.y; r.at(0, 2) = s.z;
    r.at(1, 0) = u.x; r.at(1, 1) = u.y; r.at(1, 2) = u.z;
    r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z;
    r.at(0, 3) = -dot(s, eye);
    r.at(1, 3) = -dot(u, eye);
    r.at(2, 3) = dot(f, eye);
    return r;
}

Mat4 Mat4::operator*(const Mat4& rhs) const {
    Mat4 r;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += at(row, k) * rhs.at(k, col);
            r.at(row, col) = sum;
        }
    return r;
}

Vec3 Mat4::transform_point(Vec3 p) const {
    const float x = at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z + at(0, 3);
    const float y = at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z + at(1, 3);
    const float z = at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z + at(2, 3);
    const float w = at(3, 0) * p.x + at(3, 1) * p.y + at(3, 2) * p.z + at(3, 3);
    if (w != 0 && w != 1) return Vec3{x / w, y / w, z / w};
    return Vec3{x, y, z};
}

Vec3 Mat4::transform_direction(Vec3 d) const {
    return Vec3{at(0, 0) * d.x + at(0, 1) * d.y + at(0, 2) * d.z, at(1, 0) * d.x + at(1, 1) * d.y + at(1, 2) * d.z,
                at(2, 0) * d.x + at(2, 1) * d.y + at(2, 2) * d.z};
}

} // namespace rw
