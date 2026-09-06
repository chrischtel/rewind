// Headless checks for Mat4 and the mesh builders (polylines, polygons, circles).

#include <rewind/core/math.h>
#include <rewind/render/mesh.h>

#include <cmath>
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

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

/// Total signed area of a mesh's triangles, in screen (y-down) orientation.
float mesh_area(const Mesh& m) {
    float area = 0;
    for (usize i = 0; i + 2 < m.indices.size(); i += 3) {
        const Vec3 a = m.vertices[m.indices[i]].pos, b = m.vertices[m.indices[i + 1]].pos,
                   c = m.vertices[m.indices[i + 2]].pos;
        area += std::fabs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) * 0.5f;
    }
    return area;
}

bool indices_valid(const Mesh& m) {
    for (u32 i : m.indices)
        if (i >= m.vertices.size()) return false;
    return m.indices.size() % 3 == 0;
}

void test_mat4() {
    const Mat4 t = Mat4::translation(10, 20, 30);
    const Vec3 p = t.transform_point({1, 2, 3});
    CHECK(near(p.x, 11) && near(p.y, 22) && near(p.z, 33));

    const Mat4 s = Mat4::scaling(2, 3, 4);
    const Vec3 q = (t * s).transform_point({1, 1, 1}); // scale first, then translate
    CHECK(near(q.x, 12) && near(q.y, 23) && near(q.z, 34));

    const Mat4 r = Mat4::rotation_z(3.14159265f / 2);
    const Vec3 rp = r.transform_point({1, 0, 0});
    CHECK(near(rp.x, 0) && near(rp.y, 1));

    // Pixel projection: top-left -> (-1, 1), bottom-right -> (1, -1).
    const Mat4 o = Mat4::ortho_pixels(640, 480);
    const Vec3 tl = o.transform_point({0, 0, 0});
    const Vec3 br = o.transform_point({640, 480, 0});
    CHECK(near(tl.x, -1) && near(tl.y, 1));
    CHECK(near(br.x, 1) && near(br.y, -1));

    // Perspective + look_at: a point straight ahead lands at the centre.
    const Mat4 vp = Mat4::perspective(1.0f, 1.0f, 0.1f, 100.0f) * Mat4::look_at({0, 0, 10}, {0, 0, 0}, {0, 1, 0});
    const Vec3 c = vp.transform_point({0, 0, 0});
    CHECK(near(c.x, 0) && near(c.y, 0));
    // Something further away ends up with a larger depth.
    const Vec3 far_pt = vp.transform_point({0, 0, -50});
    CHECK(far_pt.z > c.z);

    // Column-major storage: translation lives in m[12..14].
    CHECK(near(t.m[12], 10) && near(t.m[13], 20) && near(t.m[14], 30));
}

void test_polyline_basic() {
    Mesh m;
    StrokeStyle style;
    style.width = 4;
    build_polyline({{0, 0}, {100, 0}}, style, m);
    CHECK(indices_valid(m));
    CHECK(m.triangle_count() == 2);
    CHECK(near(mesh_area(m), 400));
    // u runs 0..1 along the line, v 0..1 across.
    float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
    for (const MeshVertex& v : m.vertices) {
        umin = std::min(umin, v.uv.x);
        umax = std::max(umax, v.uv.x);
        vmin = std::min(vmin, v.uv.y);
        vmax = std::max(vmax, v.uv.y);
    }
    CHECK(near(umin, 0) && near(umax, 1) && near(vmin, 0) && near(vmax, 1));

    // uv_per_unit scales u by length.
    Mesh m2;
    style.uv_per_unit = 0.1f;
    build_polyline({{0, 0}, {100, 0}}, style, m2);
    float umax2 = 0;
    for (const MeshVertex& v : m2.vertices) umax2 = std::max(umax2, v.uv.x);
    CHECK(near(umax2, 10));

    // Square caps extend the line by half the width at both ends.
    Mesh m3;
    style.uv_per_unit = 0;
    style.cap = LineCap::Square;
    build_polyline({{0, 0}, {100, 0}}, style, m3);
    CHECK(near(mesh_area(m3), 416));

    // Degenerate inputs draw nothing and do not crash.
    Mesh empty;
    build_polyline({{5, 5}}, style, empty);
    build_polyline({{5, 5}, {5, 5}}, style, empty);
    CHECK(empty.vertices.empty());
}

void test_polyline_joins() {
    const std::vector<Vec2> corner = {{0, 0}, {100, 0}, {100, 100}};
    StrokeStyle style;
    style.width = 10;

    Mesh miter;
    style.join = LineJoin::Miter;
    build_polyline(corner, style, miter);
    CHECK(indices_valid(miter));
    CHECK(miter.triangle_count() == 4);
    // A mitered right angle covers the full outer corner: 2 rectangles + the 5x5 corner... in total 100*10*2 + ...
    // Exact: two 100x10 strips overlapping in a 10x10 square, plus the miter fills that square = 2000 - 100 + 100.
    CHECK(near(mesh_area(miter), 2000, 1));

    Mesh bevel;
    style.join = LineJoin::Bevel;
    build_polyline(corner, style, bevel);
    CHECK(indices_valid(bevel));
    CHECK(bevel.triangle_count() == 5); // 2 quads + bevel triangle
    // Bevel cuts the outer corner: half the 10x10 square is missing relative to the miter... strips 2 * 100*10
    // overlap in the inner 5x5... trust the range instead of exact value.
    CHECK(mesh_area(bevel) < mesh_area(miter) && mesh_area(bevel) > 1800);

    Mesh round;
    style.join = LineJoin::Round;
    build_polyline(corner, style, round);
    CHECK(indices_valid(round));
    CHECK(round.triangle_count() > 5);

    // A sharp spike past the miter limit falls back to a bevel.
    Mesh spike;
    style.join = LineJoin::Miter;
    style.miter_limit = 2;
    build_polyline({{0, 0}, {100, 0}, {0, 5}}, style, spike);
    CHECK(indices_valid(spike));
    CHECK(spike.triangle_count() == 5);

    // Closed loop: a square outline has 4 quads and 4 joins (miters add no triangles).
    Mesh loop;
    style.miter_limit = 4;
    style.closed = true;
    build_polyline({{0, 0}, {100, 0}, {100, 100}, {0, 100}}, style, loop);
    CHECK(indices_valid(loop));
    CHECK(loop.triangle_count() == 8);

    // Round caps add fans at both ends only when open.
    Mesh caps;
    style.closed = false;
    style.cap = LineCap::Round;
    build_polyline({{0, 0}, {100, 0}}, style, caps);
    CHECK(indices_valid(caps));
    CHECK(caps.triangle_count() > 2);
    CHECK(mesh_area(caps) > 1000 && mesh_area(caps) < 1000 + 3.15f * 25 + 1);
}

void test_polyline_dashes() {
    StrokeStyle style;
    style.width = 2;
    style.dash = 10;
    style.gap = 10;
    Mesh m;
    build_polyline({{0, 0}, {100, 0}}, style, m);
    CHECK(indices_valid(m));
    // 100 units, 20 per period: 5 dashes of 10 -> 5 quads.
    CHECK(m.triangle_count() == 10);
    CHECK(near(mesh_area(m), 100, 0.5f));

    // Dashes carry on around corners.
    Mesh corner;
    build_polyline({{0, 0}, {100, 0}, {100, 100}}, style, corner);
    CHECK(indices_valid(corner));
    CHECK(near(mesh_area(corner), 200, 1));

    // Offset shifts the pattern but keeps the total length.
    Mesh shifted;
    style.dash_offset = 5;
    build_polyline({{0, 0}, {100, 0}}, style, shifted);
    CHECK(near(mesh_area(shifted), 100, 0.5f));
    CHECK(shifted.triangle_count() == 12); // half dash at start, half at end (6 pieces)
}

void test_polygon() {
    Mesh square;
    build_polygon({{0, 0}, {100, 0}, {100, 100}, {0, 100}}, 1, 1, 1, 1, square);
    CHECK(indices_valid(square));
    CHECK(square.triangle_count() == 2);
    CHECK(near(mesh_area(square), 10000));

    // Concave "L" shape, both windings.
    const std::vector<Vec2> ell = {{0, 0}, {100, 0}, {100, 50}, {50, 50}, {50, 100}, {0, 100}};
    Mesh a;
    build_polygon(ell, 1, 1, 1, 1, a);
    CHECK(indices_valid(a));
    CHECK(a.triangle_count() == 4);
    CHECK(near(mesh_area(a), 7500));

    std::vector<Vec2> reversed(ell.rbegin(), ell.rend());
    Mesh b;
    build_polygon(reversed, 1, 1, 1, 1, b);
    CHECK(b.triangle_count() == 4);
    CHECK(near(mesh_area(b), 7500));

    // Star: ear clipping must not use ears that contain other vertices.
    std::vector<Vec2> star;
    for (int i = 0; i < 10; ++i) {
        const float ang = static_cast<float>(i) / 10 * 6.2831853f;
        const float r = (i % 2 == 0) ? 100.0f : 40.0f;
        star.push_back({std::cos(ang) * r, std::sin(ang) * r});
    }
    Mesh s;
    build_polygon(star, 1, 1, 1, 1, s);
    CHECK(indices_valid(s));
    CHECK(s.triangle_count() == 8);
    CHECK(near(mesh_area(s), std::fabs(polygon_area(star)), 1));

    // uv scale maps position to uv.
    Mesh uv;
    build_polygon({{0, 0}, {10, 0}, {10, 10}}, 1, 1, 1, 1, uv, 0.1f);
    CHECK(near(uv.vertices[1].uv.x, 1) && near(uv.vertices[2].uv.y, 1));

    CHECK(polyline_length({{0, 0}, {3, 4}}) == 5);
    CHECK(near(polyline_length({{0, 0}, {3, 4}, {3, 0}}, true), 5 + 4 + 3));
}

void test_circle() {
    Mesh disc;
    build_circle({0, 0}, 10, 0, 64, 1, 1, 1, 1, disc);
    CHECK(indices_valid(disc));
    CHECK(disc.triangle_count() == 64);
    CHECK(near(mesh_area(disc), 3.14159f * 100, 2));

    Mesh ring;
    build_circle({0, 0}, 10, 5, 64, 1, 1, 1, 1, ring);
    CHECK(indices_valid(ring));
    CHECK(ring.triangle_count() == 128);
    CHECK(near(mesh_area(ring), 3.14159f * (100 - 25), 2));

    // Appending keeps earlier geometry intact.
    const usize before = ring.vertices.size();
    build_circle({50, 0}, 1, 0, 3, 1, 1, 1, 1, ring);
    CHECK(ring.vertices.size() == before + 4);
    CHECK(indices_valid(ring));
}

} // namespace

int main() {
    test_mat4();
    test_polyline_basic();
    test_polyline_joins();
    test_polyline_dashes();
    test_polygon();
    test_circle();
    if (failures == 0) std::printf("geometry_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
