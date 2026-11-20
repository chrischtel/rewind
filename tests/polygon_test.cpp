// Headless checks for areas: unions, differences, offsets, bands, clipping,
// hatching and the triangles they turn into.

#include <rewind/core/curve.h>
#include <rewind/core/polygon.h>
#include <rewind/render/mesh.h>

#include <algorithm>
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

bool near(float a, float b, float eps = 1e-2f) { return std::fabs(a - b) <= eps; }

constexpr float kPi = 3.14159265f;

Ring square(float x, float y, float size) { return {{x, y}, {x + size, y}, {x + size, y + size}, {x, y + size}}; }

float mesh_area(const Mesh& m) {
    float area = 0;
    for (usize i = 0; i + 2 < m.indices.size(); i += 3) {
        const Vec3 a = m.vertices[m.indices[i]].pos, b = m.vertices[m.indices[i + 1]].pos,
                   c = m.vertices[m.indices[i + 2]].pos;
        area += std::fabs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) * 0.5f;
    }
    return area;
}

float polyline_len(const std::vector<Vec2>& line) {
    float len = 0;
    for (usize i = 0; i + 1 < line.size(); ++i) len += distance(line[i], line[i + 1]);
    return len;
}

/// Results promise positive boundaries and negative holes.
bool well_wound(const PolygonSet& set) {
    for (const Polygon& p : set.polygons) {
        if (polygon_area(p.outer) <= 0) return false;
        for (const Ring& h : p.holes)
            if (polygon_area(h) >= 0) return false;
    }
    return true;
}

void test_union() {
    // Two overlapping squares, one wound each way: winding must not matter.
    Ring a = square(0, 0, 10);
    Ring b = square(5, 0, 10);
    std::reverse(b.begin(), b.end());
    const PolygonSet u = polygon_union(std::vector<Ring>{a, b});
    CHECK(u.size() == 1);
    CHECK(near(u.area(), 150));
    CHECK(well_wound(u));
    CHECK(u.contains({7, 5}) && u.contains({14, 5}) && !u.contains({16, 5}));

    // Apart: two regions. Touching along an edge: one.
    CHECK(polygon_union(std::vector<Ring>{square(0, 0, 4), square(10, 0, 4)}).size() == 2);
    CHECK(polygon_union(std::vector<Ring>{square(0, 0, 4), square(4, 0, 4)}).size() == 1);

    // A ring of four bars leaves a hole in the middle.
    const PolygonSet frame = polygon_union(
        std::vector<Ring>{{{0, 0}, {30, 0}, {30, 5}, {0, 5}}, {{0, 25}, {30, 25}, {30, 30}, {0, 30}},
                          {{0, 0}, {5, 0}, {5, 30}, {0, 30}}, {{25, 0}, {30, 0}, {30, 30}, {25, 30}}});
    CHECK(frame.size() == 1);
    CHECK(frame.polygons[0].holes.size() == 1);
    CHECK(near(frame.area(), 900 - 400));
    CHECK(!frame.contains({15, 15}) && frame.contains({2, 15}));
    CHECK(well_wound(frame));

    // The hole as a region of its own, and the frame with it filled.
    const PolygonSet hole = polygon_holes(frame);
    CHECK(hole.size() == 1 && near(hole.area(), 400));
    CHECK(near(polygon_fill_holes(frame).area(), 900));

    // A hole of one set must not punch through another set lying over it.
    const PolygonSet cover = PolygonSet::from_rect({10, 10, 10, 10});
    CHECK(polygon_union(frame, cover).contains({15, 15}));
    CHECK(polygon_union(std::vector<PolygonSet>{frame, cover, PolygonSet::from_rect({40, 0, 5, 5})}).contains({15, 15}));
    CHECK(near(polygon_union(std::vector<PolygonSet>{frame, cover}).area(), 600));
}

void test_boolean() {
    const PolygonSet a = PolygonSet::from_rect({0, 0, 10, 10});
    const PolygonSet b = PolygonSet::from_rect({5, 5, 10, 10});
    CHECK(near(polygon_intersection(a, b).area(), 25));
    CHECK(near(polygon_difference(a, b).area(), 75));
    CHECK(near(polygon_xor(a, b).area(), 150));
    CHECK(polygon_intersection(a, PolygonSet::from_rect({20, 20, 5, 5})).empty());

    // Cutting a window out of the middle gives a hole, not a second region.
    const PolygonSet window = polygon_difference(a, PolygonSet::from_rect({3, 3, 4, 4}));
    CHECK(window.size() == 1 && window.polygons[0].holes.size() == 1);
    CHECK(near(window.area(), 84));
    // And an island in that hole is a region again, inside the first.
    const PolygonSet island = polygon_union(window, PolygonSet::from_rect({4, 4, 2, 2}));
    CHECK(island.size() == 2);
    CHECK(near(island.area(), 88));
    CHECK(island.contains({5, 5}) && !island.contains({3.5f, 3.5f}));
}

void test_offset() {
    const PolygonSet box = PolygonSet::from_rect({0, 0, 10, 10});
    // Round corners: the square plus four strips plus a full circle of the radius.
    const PolygonSet grown = polygon_offset(box, 2);
    CHECK(near(grown.area(), 100 + 4 * 20 + kPi * 4, 0.2f));
    OffsetOptions sharp;
    sharp.join = PolyJoin::Miter;
    sharp.miter_limit = 4;
    CHECK(near(polygon_offset(box, 2, sharp).area(), 196, 0.05f));
    CHECK(near(polygon_offset(box, -2).area(), 36, 0.05f));
    CHECK(polygon_offset(box, -6).empty()); // shrunk away entirely
    CHECK(well_wound(grown));

    // Two squares a small gap apart: closing bridges it, and leaves the outside alone.
    const PolygonSet pair = polygon_union(std::vector<Ring>{square(0, 0, 10), square(11, 0, 10)});
    CHECK(pair.size() == 2);
    const PolygonSet closed = polygon_close(pair, 1.0f);
    CHECK(closed.size() == 1);
    CHECK(closed.contains({10.5f, 5}));
    CHECK(near(closed.area(), 210, 0.5f));

    // A square with a thin spike: opening removes the spike and keeps the square.
    const PolygonSet spiked = polygon_union(std::vector<Ring>{square(0, 0, 10), {{10, 4.8f}, {20, 4.8f}, {20, 5.2f}, {10, 5.2f}}});
    const PolygonSet opened = polygon_open(spiked, 0.5f);
    CHECK(opened.size() == 1);
    CHECK(!opened.contains({15, 5}));
    CHECK(opened.area() > 99 && opened.area() < 100.5f);

    // A border lies outside the set and does not overlap it.
    const PolygonSet kerb = polygon_border(box, 0.5f);
    CHECK(near(kerb.area(), 4 * 5 + kPi * 0.25f, 0.1f));
    CHECK(polygon_intersection(kerb, box).area() < 0.01f);
    CHECK(kerb.polygons.size() == 1 && kerb.polygons[0].holes.size() == 1);
}

void test_band() {
    // A straight band is a rectangle.
    const PolygonSet lane = polyline_band({{0, 0}, {100, 0}}, 3.5f);
    CHECK(near(lane.area(), 350, 0.05f));
    CHECK(lane.contains({50, 1.7f}) && !lane.contains({50, 1.8f}) && !lane.contains({-0.1f, 0}));
    BandOptions round;
    round.cap = PolyCap::Round;
    CHECK(near(polyline_band({{0, 0}, {100, 0}}, 4, round).area(), 400 + kPi * 4, 0.4f));
    BandOptions square_cap;
    square_cap.cap = PolyCap::Square;
    CHECK(near(polyline_band({{0, 0}, {100, 0}}, 4, square_cap).area(), 416, 0.1f));

    // A band along a curve keeps its width: area is length times width.
    Path arc;
    arc.move_to({50, 0});
    arc.arc_to({0, 0}, kPi * 0.5f);
    std::vector<Vec2> line;
    arc.flatten(0.01f, line);
    const PolygonSet curved = polyline_band(line, 4);
    CHECK(near(curved.area(), 50 * kPi * 0.5f * 4, 0.6f));

    // A closed line gives a ring with a hole.
    const PolygonSet ring = polyline_band(PolygonSet::from_circle({0, 0}, 20, 64).polygons[0].outer, 2, {}, true);
    CHECK(ring.size() == 1 && ring.polygons[0].holes.size() == 1);
    CHECK(!ring.contains({0, 0}) && ring.contains({20, 0}));

    // Two lanes crossing sweep one cross-shaped area.
    const PolygonSet cross = polygon_union(polyline_band({{-20, 0}, {20, 0}}, 4), polyline_band({{0, -20}, {0, 20}}, 4));
    CHECK(cross.size() == 1);
    CHECK(near(cross.area(), 160 + 160 - 16, 0.05f));
}

void test_clip_polyline() {
    const PolygonSet box = PolygonSet::from_rect({10, -5, 10, 10});
    const std::vector<Vec2> line = {{0, 0}, {40, 0}};
    const auto inside = clip_polyline(line, box, true);
    CHECK(inside.size() == 1);
    CHECK(near(polyline_len(inside[0]), 10));
    CHECK(near(inside[0].front().x, 10) && near(inside[0].back().x, 20));

    // Outside: two pieces, in order along the line and running the same way.
    const auto outside = clip_polyline(line, box, false);
    CHECK(outside.size() == 2);
    CHECK(near(outside[0].front().x, 0) && near(outside[0].back().x, 10));
    CHECK(near(outside[1].front().x, 20) && near(outside[1].back().x, 40));

    // Several areas along the way.
    const PolygonSet gates = polygon_union(std::vector<Ring>{square(5, -2, 4), square(15, -2, 4), square(25, -2, 4)});
    const auto pieces = clip_polyline(line, gates, false);
    CHECK(pieces.size() == 4);
    float total = 0;
    for (const auto& p : pieces) total += polyline_len(p);
    CHECK(near(total, 28));
    for (usize i = 0; i + 1 < pieces.size(); ++i) CHECK(pieces[i].back().x <= pieces[i + 1].front().x + 0.01f);

    CHECK(clip_polyline(line, PolygonSet{}, true).empty());
    CHECK(clip_polyline(line, PolygonSet{}, false).size() == 1);
}

void test_hatch() {
    // Stripes 1 wide every 4: a quarter of the area, whatever the angle.
    const PolygonSet box = PolygonSet::from_rect({0, 0, 40, 40});
    CHECK(near(polygon_hatch(box, 0, 4, 1).area(), 400, 12));
    const PolygonSet diagonal = polygon_hatch(box, kPi * 0.25f, 4, 1);
    CHECK(near(diagonal.area(), 400, 12));
    CHECK(diagonal.size() >= 10);
    // Never outside the area it hatches.
    CHECK(polygon_difference(diagonal, box).area() < 0.01f);

    // An inset keeps the stripes off the border.
    const PolygonSet inset = polygon_hatch(box, kPi * 0.25f, 4, 1, 2.0f);
    CHECK(polygon_difference(inset, PolygonSet::from_rect({2, 2, 36, 36})).area() < 0.05f);

    // A triangle: stripes end on its slanted side.
    const PolygonSet wedge = PolygonSet::from_ring({{0, 0}, {30, 0}, {0, 10}});
    const PolygonSet stripes = polygon_hatch(wedge, kPi * 0.5f, 3, 1);
    CHECK(stripes.size() >= 8);
    CHECK(polygon_difference(stripes, wedge).area() < 0.01f);

    const auto lines = polygon_hatch_lines(box, 0, 4);
    CHECK(lines.size() >= 9 && lines.size() <= 11);
    for (const auto& l : lines) CHECK(near(polyline_len(l), 40, 0.05f));
}

void test_tidy() {
    // A circle of many points simplifies to far fewer and keeps its area.
    const PolygonSet circle = PolygonSet::from_circle({0, 0}, 10, 256);
    const PolygonSet simple = polygon_simplify(circle, 0.05f);
    CHECK(simple.point_count() < circle.point_count() / 3);
    CHECK(near(simple.area(), circle.area(), 2.0f));

    const PolygonSet mixed = polygon_union(std::vector<Ring>{square(0, 0, 10), square(20, 0, 0.5f)});
    CHECK(mixed.size() == 2);
    CHECK(polygon_remove_small(mixed, 1.0f).size() == 1);
    const PolygonSet pinhole = polygon_difference(PolygonSet::from_rect({0, 0, 10, 10}), PolygonSet::from_rect({4, 4, 0.5f, 0.5f}));
    CHECK(pinhole.polygons[0].holes.size() == 1);
    CHECK(polygon_remove_small(pinhole, 1.0f).polygons[0].holes.empty());

    // The widest place in an L is in its thick arm.
    const PolygonSet ell = PolygonSet::from_ring({{0, 0}, {20, 0}, {20, 4}, {8, 4}, {8, 20}, {0, 20}});
    const InnerCircle c = polygon_inner_circle(ell.polygons[0], 0.05f);
    CHECK(near(c.radius, 4.0f, 0.7f));
    CHECK(c.radius >= 4.0f - 0.1f);
    CHECK(ell.contains(c.center));
    // A long thin triangle is narrow however long it is.
    const InnerCircle sliver = polygon_inner_circle(PolygonSet::from_ring({{0, 0}, {40, 0}, {0, 2}}).polygons[0], 0.02f);
    CHECK(sliver.radius < 1.0f);

    const PolygonSet box = PolygonSet::from_rect({0, 0, 10, 10});
    CHECK(near(polygon_signed_distance(box, {5, 5}), -5));
    CHECK(near(polygon_signed_distance(box, {13, 5}), 3));
    CHECK(near(box.bounds().w, 10) && near(box.bounds().h, 10));
}

void test_mesh() {
    // Triangles cover exactly the area, holes left open.
    const PolygonSet window = polygon_difference(PolygonSet::from_rect({0, 0, 10, 10}), PolygonSet::from_rect({3, 3, 4, 4}));
    Mesh mesh;
    build_polygon_set(window, 1, 1, 1, 1, mesh);
    CHECK(mesh.triangle_count() >= 8);
    CHECK(near(mesh_area(mesh), 84, 0.05f));
    for (u32 i : mesh.indices) CHECK(i < mesh.vertices.size());

    // No triangle reaches into the hole.
    bool clear = true;
    for (usize i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const Vec3 a = mesh.vertices[mesh.indices[i]].pos, b = mesh.vertices[mesh.indices[i + 1]].pos,
                   c = mesh.vertices[mesh.indices[i + 2]].pos;
        const Vec2 mid{(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3};
        if (mid.x > 3.01f && mid.x < 6.99f && mid.y > 3.01f && mid.y < 6.99f) clear = false;
    }
    CHECK(clear);

    // Several regions, appended after what is already there.
    const usize before = mesh.vertices.size();
    build_polygon_set(polygon_union(std::vector<Ring>{square(20, 0, 5), square(30, 0, 5)}), 1, 0, 0, 1, mesh);
    CHECK(mesh.vertices.size() == before + 8);
    CHECK(near(mesh_area(mesh), 84 + 50, 0.05f));

    Mesh outline;
    StrokeStyle style;
    style.width = 0.2f;
    build_polygon_outline(window, style, outline);
    CHECK(near(mesh_area(outline), (40 + 16) * 0.2f, 0.2f));
}

void test_determinism() {
    // The same shapes in give the same points out, every time.
    const auto make = [] {
        std::vector<PolygonSet> bands;
        for (int i = 0; i < 12; ++i) {
            const float a = static_cast<float>(i) * 0.5236f;
            bands.push_back(polyline_band({{0, 0}, {std::cos(a) * 30, std::sin(a) * 30}}, 3.5f));
        }
        return polygon_close(polygon_union(bands), 1.5f);
    };
    const PolygonSet first = make();
    const PolygonSet second = make();
    CHECK(first.size() == second.size() && first.point_count() == second.point_count());
    bool same = first.size() == second.size();
    for (usize i = 0; same && i < first.size(); ++i) {
        const Ring& a = first.polygons[i].outer;
        const Ring& b = second.polygons[i].outer;
        same = a.size() == b.size();
        for (usize k = 0; same && k < a.size(); ++k) same = a[k].x == b[k].x && a[k].y == b[k].y;
    }
    CHECK(same);
    CHECK(first.size() == 1);
}

} // namespace

int main() {
    test_union();
    test_boolean();
    test_offset();
    test_band();
    test_clip_polyline();
    test_hatch();
    test_tidy();
    test_mesh();
    test_determinism();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("polygon_test: all passed\n");
    return 0;
}
