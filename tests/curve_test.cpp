// Headless checks for core/curve.h: Beziers, arcs, paths, sampling, offsets.

#include <rewind/core/curve.h>
#include <rewind/core/math.h>
#include <rewind/sim/reflect.h>
#include <rewind/sim/serialize.h>

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
bool near(Vec2 a, Vec2 b, float eps = 1e-3f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps); }

constexpr float kPi = 3.14159265f;

void test_vec2() {
    const Vec2 a{3, 4};
    CHECK(near(length(a), 5));
    CHECK(near(normalize(a), {0.6f, 0.8f}));
    CHECK(near(rotate({1, 0}, kPi / 2), {0, 1}));
    CHECK(near(angle_of({0, 1}), kPi / 2));
    CHECK(near(perp({1, 0}), {0, 1}));
    CHECK(near(lerp({0, 0}, {10, 0}, 0.25f), {2.5f, 0}));
    CHECK(near(cross({1, 0}, {0, 1}), 1));
}

void test_bezier() {
    const CubicBezier line = CubicBezier::line({0, 0}, {100, 0});
    CHECK(near(line.point(0.5f), {50, 0}));
    CHECK(near(line.length(), 100));
    CHECK(near(line.direction(0.3f), {1, 0}));
    CHECK(near(line.curvature(0.5f), 0));

    // A symmetric curve: midpoint on the axis, tangent horizontal there.
    const CubicBezier bump{{0, 0}, {30, -100}, {70, -100}, {100, 0}};
    CHECK(near(bump.point(0.5f).x, 50));
    CHECK(near(bump.direction(0.5f), {1, 0}));
    CHECK(bump.point(0.5f).y < -70);
    CHECK(bump.curvature(0.5f) > 0 || bump.curvature(0.5f) < 0); // non-zero either way

    // Split halves have the same total length, and rejoin at the split point.
    CubicBezier a, b;
    bump.split(0.3f, a, b);
    CHECK(near(a.p3, bump.point(0.3f)));
    CHECK(near(a.length() + b.length(), bump.length(), 0.05f));
    CHECK(near(a.point(1), b.point(0)));
    const CubicBezier mid = bump.sub_curve(0.25f, 0.75f);
    CHECK(near(mid.p0, bump.point(0.25f)) && near(mid.p3, bump.point(0.75f)));

    // Bounds include the extreme of the bump, not just the end points.
    const Rect r = bump.bounds();
    CHECK(near(r.x, 0) && near(r.right(), 100));
    CHECK(r.y < -70 && near(r.bottom(), 0));

    // Quadratic conversion preserves the midpoint.
    const CubicBezier q = CubicBezier::from_quadratic({0, 0}, {50, 100}, {100, 0});
    CHECK(near(q.point(0.5f), {50, 50}));

    // A quarter arc as a Bezier stays within 0.1% of the radius.
    const CubicBezier arc = CubicBezier::arc({0, 0}, 100, 0, kPi / 2);
    CHECK(near(arc.p0, {100, 0}) && near(arc.p3, {0, 100}));
    for (int i = 0; i <= 10; ++i) CHECK(near(length(arc.point(i / 10.0f)), 100, 0.1f));
    CHECK(near(arc.length(), kPi * 50, 0.05f));
    CHECK(near(arc.reversed().point(0), {0, 100}));
}

void test_arc() {
    Arc arc;
    arc.center = {10, 10};
    arc.radius = 5;
    arc.start = 0;
    arc.sweep = kPi;
    CHECK(near(arc.point(0), {15, 10}) && near(arc.point(1), {5, 10}));
    CHECK(near(arc.length(), kPi * 5));
    CHECK(near(arc.direction(0), {0, 1}));
    arc.sweep = -kPi;
    CHECK(near(arc.direction(0), {0, -1}));

    std::vector<CubicBezier> pieces;
    arc.sweep = 2 * kPi;
    arc_to_beziers(arc, pieces);
    CHECK(pieces.size() == 4);
    float total = 0;
    for (const CubicBezier& p : pieces) total += p.length();
    CHECK(near(total, 2 * kPi * 5, 0.05f));
    CHECK(near(pieces.front().p0, pieces.back().p3, 1e-3f));
}

void test_path_building() {
    Path p;
    p.move_to({0, 0});
    p.line_to({100, 0});
    p.arc_to({100, 50}, kPi); // half circle down to (100, 100)
    p.line_to({0, 100});
    p.close();
    CHECK(p.closed);
    CHECK(p.segments.size() == 5); // line, 2 arc pieces, line, closing line
    CHECK(near(p.end(), p.start()));
    CHECK(near(p.length(), 100 + kPi * 50 + 100 + 100, 0.1f));
    const Rect b = p.bounds();
    CHECK(near(b.x, 0) && near(b.right(), 150, 0.01f) && near(b.y, 0) && near(b.bottom(), 100));

    Path poly = Path::from_polyline({{0, 0}, {10, 0}, {10, 10}}, true);
    CHECK(poly.segments.size() == 3 && poly.closed);

    // Catmull-Rom passes through every point with a horizontal tangent at the top of a symmetric hump.
    const std::vector<Vec2> pts = {{0, 0}, {50, -50}, {100, 0}, {150, 50}, {200, 0}};
    Path cr = Path::from_catmull_rom(pts, false);
    CHECK(cr.segments.size() == 4);
    for (usize i = 0; i < cr.segments.size(); ++i) CHECK(near(cr.segments[i].p0, pts[i]) && near(cr.segments[i].p3, pts[i + 1]));
    CHECK(near(cr.direction(1, 0), {1, 0}, 0.05f)); // at (50,-50) the curve runs horizontally
    Path cr_closed = Path::from_catmull_rom({{0, 0}, {100, 0}, {100, 100}, {0, 100}}, true);
    CHECK(cr_closed.segments.size() == 4 && cr_closed.closed);
    CHECK(near(cr_closed.end(), cr_closed.start()));
}

void test_path_editing() {
    Path p = Path::from_polyline({{0, 0}, {100, 0}, {100, 100}});
    Path a, b;
    p.split(0, 0.5f, a, b);
    CHECK(a.segments.size() == 1 && b.segments.size() == 2);
    CHECK(near(a.end(), {50, 0}) && near(b.start(), {50, 0}));
    CHECK(near(a.length() + b.length(), p.length()));

    p.subdivide(1, 0.25f);
    CHECK(p.segments.size() == 3);
    CHECK(near(p.segments[1].p3, {100, 25}));
    CHECK(near(p.length(), 200));

    Path q = Path::from_polyline({{200, 100}, {300, 100}});
    Path joined = p;
    joined.join(q); // bridge from (100,100) to (200,100)
    CHECK(joined.segments.size() == 5);
    CHECK(near(joined.length(), 400));

    Path rev = p.reversed();
    CHECK(near(rev.start(), {100, 100}) && near(rev.end(), {0, 0}));

    Path loop = Path::from_polyline({{0, 0}, {100, 0}, {100, 100}, {0, 100}}, true);
    Path cut, unused;
    loop.split(2, 0.5f, cut, unused);
    CHECK(!cut.closed && cut.segments.size() == 5);
    CHECK(near(cut.start(), {50, 100}) && near(cut.end(), {50, 100}));

    Path moved = p;
    moved.transform(Mat4::translation(10, 20));
    CHECK(near(moved.start(), {10, 20}));
}

void test_flatten_and_sampler() {
    Path circle;
    circle.move_to({100, 0});
    circle.arc_to({0, 0}, 2 * kPi);
    circle.close();
    std::vector<Vec2> flat;
    circle.flatten(0.1f, flat);
    CHECK(flat.size() > 30);
    for (const Vec2& v : flat) CHECK(near(length(v), 100, 0.2f));
    std::vector<Vec2> coarse;
    circle.flatten(5.0f, coarse);
    CHECK(coarse.size() < flat.size());

    PathSampler s(circle, 0.05f);
    CHECK(s.closed());
    CHECK(near(s.length(), 2 * kPi * 100, 0.5f));
    const PathPoint quarter = s.at(kPi * 50);
    CHECK(near(quarter.position, {0, 100}, 0.5f));
    CHECK(near(quarter.direction, {-1, 0}, 0.02f));
    CHECK(near(s.at(s.length() + kPi * 50).position, quarter.position, 0.5f)); // wraps
    CHECK(near(s.at(-kPi * 50).position, {0, -100}, 0.5f));

    // Closest point: radial projection onto the circle. The Bezier arcs sit
    // within 0.03% of the true circle, which is enough to shift the exact
    // minimum a unit or so along the curve, so the tolerance is loose.
    const PathPoint c = s.closest({30, 40});
    CHECK(near(c.position, {60, 80}, 1.5f));
    CHECK(near(s.distance_to({30, 40}), 50, 0.1f));
    CHECK(near(c.distance, std::atan2(80.0f, 60.0f) * 100, 2.0f));
    CHECK(near(length(c.position), 100, 0.1f));

    // Open path: clamps and hits the exact end points.
    Path open = Path::from_polyline({{0, 0}, {100, 0}, {100, 100}});
    PathSampler os(open);
    CHECK(near(os.at(-5).position, {0, 0}));
    CHECK(near(os.at(150).position, {100, 50}));
    CHECK(near(os.at(150).direction, {0, 1}));
    CHECK(near(os.at(999).position, {100, 100}));
    CHECK(near(os.closest({100, 40}).distance, 140));
    CHECK(near(os.closest({120, 120}).position, {100, 100}));
    CHECK(os.points().size() == 3);
}

void test_offsets() {
    const std::vector<Vec2> square = {{0, 0}, {100, 0}, {100, 100}, {0, 100}};
    // In y-down space perp() points right of travel; walking a clockwise
    // square on screen, the right side is inside. Negative grows it.
    std::vector<Vec2> grown = offset_polyline(square, -10, true);
    CHECK(grown.size() == 4);
    CHECK(near(grown[0], {-10, -10}) && near(grown[2], {110, 110}));
    std::vector<Vec2> shrunk = offset_polyline(square, 10, true);
    CHECK(near(shrunk[0], {10, 10}) && near(shrunk[2], {90, 90}));

    // Open polyline: end points shift sideways, the corner is mitered.
    std::vector<Vec2> off = offset_polyline({{0, 0}, {100, 0}, {100, 100}}, 5, false);
    CHECK(off.size() == 3);
    CHECK(near(off[0], {0, 5}) && near(off[1], {95, 5}) && near(off[2], {95, 100}));

    // Very sharp corner: the miter is limited and becomes a bevel (one extra point).
    std::vector<Vec2> sharp = offset_polyline({{0, 0}, {100, 0}, {0, 5}}, -5, false, 2);
    CHECK(sharp.size() == 4);

    // Path offset runs through flatten: the offset of a circle is a circle.
    Path circle;
    circle.move_to({100, 0});
    circle.arc_to({0, 0}, 2 * kPi);
    circle.close();
    for (const Vec2& v : circle.offset(-20, 0.05f)) CHECK(near(length(v), 120, 0.3f));

    // Simplify keeps corners, drops collinear points.
    std::vector<Vec2> simple = simplify_polyline({{0, 0}, {50, 0.1f}, {100, 0}, {100, 50}, {100, 100}}, 0.5f);
    CHECK(simple.size() == 3);

    std::vector<Vec2> re = resample_polyline({{0, 0}, {100, 0}}, 25);
    CHECK(re.size() == 5 && near(re[2], {50, 0}));
    std::vector<Vec2> re2 = resample_polyline({{0, 0}, {30, 0}, {30, 40}}, 10);
    CHECK(re2.size() == 8 && near(re2.back(), {30, 40}));
}

void test_predicates() {
    float t;
    CHECK(near(closest_point_on_segment({0, 0}, {10, 0}, {5, 5}, &t), {5, 0}) && near(t, 0.5f));
    CHECK(near(closest_point_on_segment({0, 0}, {10, 0}, {20, 5}), {10, 0}));
    Vec2 x;
    CHECK(line_intersection({0, 0}, {1, 0}, {5, -5}, {0, 1}, x) && near(x, {5, 0}));
    CHECK(!line_intersection({0, 0}, {1, 0}, {0, 5}, {1, 0}, x));
    CHECK(segments_intersect({0, 0}, {10, 10}, {0, 10}, {10, 0}, &x) && near(x, {5, 5}));
    CHECK(!segments_intersect({0, 0}, {1, 1}, {5, 5}, {6, 6}));
    CHECK(segments_intersect({0, 0}, {4, 4}, {2, 2}, {6, 6}));

    const std::vector<Vec2> ell = {{0, 0}, {100, 0}, {100, 50}, {50, 50}, {50, 100}, {0, 100}};
    CHECK(polygon_contains(ell, {25, 75}));
    CHECK(!polygon_contains(ell, {75, 75}));
    CHECK(polygon_contains(ell, {75, 25}));

    std::vector<Vec2> hull = convex_hull({{0, 0}, {100, 0}, {100, 100}, {0, 100}, {50, 50}, {20, 30}});
    CHECK(hull.size() == 4);
}

void test_reflection() {
    Path p = Path::from_catmull_rom({{0, 0}, {50, 20}, {100, 0}});
    p.closed = true;
    Writer w;
    {
        SaveVisitor save(w);
        p.reflect(save);
    }
    Path q;
    {
        Reader r(w.bytes());
        LoadVisitor load(r);
        q.reflect(load);
    }
    CHECK(q.closed && q.segments.size() == 2);
    CHECK(near(q.segments[1].p1, p.segments[1].p1));
}

} // namespace

void test_fit() {
    // A straight stroke is one segment with exact ends.
    std::vector<Vec2> line;
    for (int i = 0; i <= 10; ++i) line.push_back({static_cast<float>(i) * 10, static_cast<float>(i) * 5});
    Path p = fit_path(line, 0.5f);
    CHECK(p.segments.size() == 1);
    CHECK(distance(p.start(), line.front()) < 1e-4f && distance(p.end(), line.back()) < 1e-4f);

    // A dense sine wave: few segments, every sample within tolerance.
    std::vector<Vec2> wave;
    for (int i = 0; i <= 200; ++i) {
        const float x = static_cast<float>(i) * 2;
        wave.push_back({x, std::sin(x / 40) * 50});
    }
    Path w = fit_path(wave, 1.0f);
    CHECK(w.segments.size() >= 2 && w.segments.size() <= 14);
    PathSampler s(w, 0.05f);
    float worst = 0;
    for (const Vec2& v : wave) worst = std::max(worst, s.distance_to(v));
    CHECK(worst < 1.2f);
    // A looser tolerance needs fewer pieces.
    CHECK(fit_path(wave, 8.0f).segments.size() < w.segments.size());

    // Degenerate input.
    CHECK(fit_path({}, 1).empty());
    CHECK(fit_path({{1, 1}, {1, 1}, {1, 1}}, 1).empty());
    CHECK(fit_path({{0, 0}, {0, 0}, {10, 0}}, 1).segments.size() == 1);

    // A circle drawn as a closed stroke comes back closed.
    std::vector<Vec2> circle;
    for (int i = 0; i < 36; ++i) {
        const float a = static_cast<float>(i) / 36 * 6.2831853f;
        circle.push_back({std::cos(a) * 100, std::sin(a) * 100});
    }
    Path c = fit_path(circle, 0.5f, true);
    CHECK(c.closed && c.segments.size() >= 3 && distance(c.start(), c.end()) < 1e-3f);
}

void test_sub_path_single_segment() {
    // Both cuts inside one segment: the head cut has to be rescaled to the
    // piece that is left after the tail cut, or it lands short.
    const Path line = Path::from_polyline({{0, 0}, {100, 0}});
    const Path mid = line.sub_path(10, 90);
    CHECK(near(mid.start().x, 10, 0.05f));
    CHECK(near(mid.end().x, 90, 0.05f));
    CHECK(near(mid.length(), 80, 0.1f));
    const Path head = line.sub_path(0, 30);
    CHECK(near(head.end().x, 30, 0.05f));
    const Path tail = line.sub_path(70, 100);
    CHECK(near(tail.start().x, 70, 0.05f));
}

int main() {
    test_fit();
    test_vec2();
    test_bezier();
    test_arc();
    test_path_building();
    test_path_editing();
    test_flatten_and_sampler();
    test_offsets();
    test_sub_path_single_segment();
    test_predicates();
    test_reflection();
    if (failures == 0) std::printf("curve_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
