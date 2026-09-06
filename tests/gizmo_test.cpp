// Headless checks for the immediate-mode gizmos: hover, drag, snapping,
// axis constraints, rectangles, rotation, path tangents, and the visitor
// that finds gizmo-able fields in reflected state.

#include <rewind/debug/gizmo.h>

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

const Camera kCam{0, 0, 1}; // world == screen

/// One frame: mouse at `m`, button state `down`, run `body` between begin and end.
template <class F> void frame(Gizmos& g, Vec2 m, bool down, F&& body, bool shift = false, bool alt = false) {
    GizmoInput in;
    in.mouse = m;
    in.down = down;
    in.shift = shift;
    in.alt = alt;
    g.begin(in, kCam, 800, 600);
    body();
    g.end();
}

void test_point_drag() {
    Gizmos g;
    Vec2 p{100, 100};
    bool changed = false;
    // Hover, press, drag, release.
    frame(g, {101, 100}, false, [&] { changed = g.point(1, p); });
    CHECK(g.hot() == 1 && !changed);
    frame(g, {101, 100}, true, [&] { changed = g.point(1, p); });
    CHECK(g.active() == 1 && !changed);
    frame(g, {151, 130}, true, [&] { changed = g.point(1, p); });
    CHECK(changed && near(p, {150, 130})); // grab offset kept: no jump
    frame(g, {151, 130}, false, [&] { changed = g.point(1, p); });
    CHECK(!g.dragging() && !changed);
    CHECK(!g.shapes().empty());

    // Miss: a press away from any handle is a click on empty space.
    frame(g, {400, 400}, false, [&] { g.point(1, p); });
    frame(g, {400, 400}, true, [&] { g.point(1, p); });
    CHECK(g.clicked_empty() && g.active() == 0);
    frame(g, {400, 400}, false, [&] { g.point(1, p); });

    // Disabled input never hovers.
    GizmoInput in;
    in.mouse = p;
    in.enabled = false;
    g.begin(in, kCam, 800, 600);
    g.point(1, p);
    g.end();
    CHECK(g.hot() == 0);
}

void test_snapping_and_axis() {
    Gizmos g;
    g.snap().enabled = true;
    g.snap().grid = 10;
    Vec2 p{100, 100};
    frame(g, {100, 100}, false, [&] { g.point(1, p); });
    frame(g, {100, 100}, true, [&] { g.point(1, p); });
    frame(g, {123, 138}, true, [&] { g.point(1, p); });
    CHECK(near(p, {120, 140}));
    // Alt bypasses the grid.
    frame(g, {123, 138}, true, [&] { g.point(1, p); }, false, true);
    CHECK(near(p, {123, 138}));
    frame(g, {123, 138}, false, [&] { g.point(1, p); });

    // Shift locks to the dominant axis.
    g.snap().enabled = false;
    p = {100, 100};
    frame(g, {100, 100}, false, [&] { g.point(2, p); });
    frame(g, {100, 100}, true, [&] { g.point(2, p); });
    frame(g, {160, 110}, true, [&] { g.point(2, p); }, true);
    CHECK(near(p, {160, 100}));
    frame(g, {160, 110}, false, [&] { g.point(2, p); });

    CHECK(near(g.snap_angle(0.3f), 0.3f)); // disabled
    g.snap().enabled = true;
    g.snap().angle_deg = 90;
    CHECK(near(g.snap_angle(1.4f), 3.14159265f / 2));
}

void test_move_gizmo() {
    Gizmos g;
    Vec2 p{200, 200};
    // Grab the x tip (48 px right) and drag diagonally: only x changes.
    frame(g, {248, 200}, false, [&] { g.move(5, p); });
    CHECK(g.hot() == 5);
    frame(g, {248, 200}, true, [&] { g.move(5, p); });
    frame(g, {278, 260}, true, [&] { g.move(5, p); });
    CHECK(near(p, {230, 200}));
    frame(g, {278, 260}, false, [&] { g.move(5, p); });
    // Centre square moves freely.
    frame(g, {230, 200}, false, [&] { g.move(5, p); });
    frame(g, {230, 200}, true, [&] { g.move(5, p); });
    frame(g, {240, 220}, true, [&] { g.move(5, p); });
    CHECK(near(p, {240, 220}));
    frame(g, {240, 220}, false, [&] { g.move(5, p); });
}

void test_rect() {
    Gizmos g;
    Rect r{100, 100, 50, 40};
    // Bottom-right corner.
    frame(g, {150, 140}, false, [&] { g.rect(7, r); });
    frame(g, {150, 140}, true, [&] { g.rect(7, r); });
    frame(g, {170, 150}, true, [&] { g.rect(7, r); });
    CHECK(near(r.x, 100) && near(r.y, 100) && near(r.w, 70) && near(r.h, 50));
    frame(g, {170, 150}, false, [&] { g.rect(7, r); });
    // Right edge midpoint.
    frame(g, {170, 125}, false, [&] { g.rect(7, r); });
    frame(g, {170, 125}, true, [&] { g.rect(7, r); });
    frame(g, {180, 125}, true, [&] { g.rect(7, r); });
    CHECK(near(r.w, 80) && near(r.h, 50));
    frame(g, {180, 125}, false, [&] { g.rect(7, r); });
    // Drag the left edge past the right one: the rect flips instead of going negative.
    frame(g, {100, 125}, false, [&] { g.rect(7, r); });
    frame(g, {100, 125}, true, [&] { g.rect(7, r); });
    frame(g, {200, 125}, true, [&] { g.rect(7, r); });
    CHECK(near(r.x, 180) && near(r.w, 20) && r.w >= 0);
    frame(g, {200, 125}, false, [&] { g.rect(7, r); });
    // Centre moves.
    const Vec2 c{r.x + r.w / 2, r.y + r.h / 2};
    frame(g, c, false, [&] { g.rect(7, r); });
    frame(g, c, true, [&] { g.rect(7, r); });
    frame(g, c + Vec2{10, 10}, true, [&] { g.rect(7, r); });
    CHECK(near(r.x, 190) && near(r.y, 110) && near(r.w, 20));
    frame(g, c + Vec2{10, 10}, false, [&] { g.rect(7, r); });
}

void test_rotate() {
    Gizmos g;
    float angle = 0;
    const Vec2 c{300, 300};
    frame(g, {348, 300}, false, [&] { g.rotate(9, c, angle); });
    CHECK(g.hot() == 9);
    frame(g, {348, 300}, true, [&] { g.rotate(9, c, angle); });
    frame(g, {300, 348}, true, [&] { g.rotate(9, c, angle); });
    CHECK(near(angle, 3.14159265f / 2, 1e-3f));
    g.snap().enabled = true;
    g.snap().angle_deg = 45;
    frame(g, {260, 335}, true, [&] { g.rotate(9, c, angle); });
    CHECK(near(angle, 3 * 3.14159265f / 4, 1e-3f));
    frame(g, {260, 335}, false, [&] { g.rotate(9, c, angle); });
}

void test_path() {
    Gizmos g;
    Path p = Path::from_polyline({{0, 0}, {100, 0}, {200, 0}});
    // Middle anchor drags with both its tangents.
    const Vec2 t_before_in = p.segments[0].p2, t_before_out = p.segments[1].p1;
    frame(g, {100, 0}, false, [&] { g.path(3, p); });
    frame(g, {100, 0}, true, [&] { g.path(3, p); });
    frame(g, {100, 50}, true, [&] { g.path(3, p); });
    CHECK(near(p.segments[0].p3, {100, 50}) && near(p.segments[1].p0, {100, 50}));
    CHECK(near(p.segments[0].p2, t_before_in + Vec2{0, 50}) && near(p.segments[1].p1, t_before_out + Vec2{0, 50}));
    frame(g, {100, 50}, false, [&] { g.path(3, p); });

    // Dragging the outgoing tangent of segment 1 mirrors segment 0's incoming tangent.
    const Vec2 t_out = p.segments[1].p1;
    const float opposite_len = length(p.segments[0].p2 - p.segments[0].p3);
    frame(g, t_out, false, [&] { g.path(3, p); });
    frame(g, t_out, true, [&] { g.path(3, p); });
    frame(g, {150, 100}, true, [&] { g.path(3, p); });
    CHECK(near(p.segments[1].p1, {150, 100}));
    const Vec2 anchor = p.segments[1].p0;
    const Vec2 dir_out = normalize(p.segments[1].p1 - anchor);
    const Vec2 dir_in = normalize(p.segments[0].p2 - anchor);
    CHECK(near(dir_in, -dir_out, 1e-3f));
    CHECK(near(length(p.segments[0].p2 - anchor), opposite_len, 1e-3f));
    frame(g, {150, 100}, false, [&] { g.path(3, p); });

    // Free end of an open path moves alone with its tangent.
    frame(g, {200, 0}, false, [&] { g.path(3, p); });
    frame(g, {200, 0}, true, [&] { g.path(3, p); });
    frame(g, {220, 20}, true, [&] { g.path(3, p); });
    CHECK(near(p.segments[1].p3, {220, 20}));
    frame(g, {220, 20}, false, [&] { g.path(3, p); });

    // Closed path: the first anchor is shared with the last segment.
    Path loop = Path::from_polyline({{0, 0}, {100, 0}, {100, 100}, {0, 100}}, true);
    frame(g, {0, 0}, false, [&] { g.path(4, loop); });
    frame(g, {0, 0}, true, [&] { g.path(4, loop); });
    frame(g, {-10, -10}, true, [&] { g.path(4, loop); });
    CHECK(near(loop.segments[0].p0, {-10, -10}) && near(loop.segments[3].p3, {-10, -10}));
    frame(g, {-10, -10}, false, [&] { g.path(4, loop); });
}

struct Thing {
    Vec2 pos{10, 10};
    Rect area{0, 0, 20, 20};
    Path route = Path::from_polyline({{0, 0}, {50, 0}});
    float speed = 3;
    void reflect(Visitor& v) {
        v.field("pos", pos);
        v.field("area", area, {.read_only = true});
        v.field("route", route);
        v.field("speed", speed);
    }
};

void test_visitor() {
    Thing t;
    GizmoVisitor v;
    t.reflect(v);
    CHECK(v.targets().size() == 3);
    CHECK(v.targets()[0].kind == Widget::Vec2 && v.targets()[0].floats.size() == 2);
    CHECK(v.targets()[1].kind == Widget::Rect && v.targets()[1].read_only);
    CHECK(v.targets()[2].kind == Widget::Path && v.targets()[2].floats.size() == 8 && v.targets()[2].closed == &t.route.closed);

    // Drive the position through the visitor: point gizmo at id_base + 1.
    Gizmos g;
    frame(g, {10, 10}, false, [&] { v.apply(g, 100, false); });
    CHECK(g.hot() == 101);
    frame(g, {10, 10}, true, [&] { v.apply(g, 100, false); });
    bool changed = false;
    frame(g, {30, 15}, true, [&] { changed = v.apply(g, 100, false); });
    CHECK(changed && near(t.pos, {30, 15}));
    frame(g, {30, 15}, false, [&] { v.apply(g, 100, false); });
    // The read-only rect never registers a handle.
    frame(g, {0, 0}, false, [&] { v.apply(g, 100, false); });
    CHECK(g.hot() != 102);
}

} // namespace

int main() {
    test_point_drag();
    test_snapping_and_axis();
    test_move_gizmo();
    test_rect();
    test_rotate();
    test_path();
    test_visitor();
    if (failures == 0) std::printf("gizmo_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
