// Headless checks for CameraController and Camera conversions.

#include <rewind/render/camera.h>

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

void settle(CameraController& c) {
    for (int i = 0; i < 600; ++i) c.update(1.0f / 60);
}

void test_camera_struct() {
    const Camera cam = Camera::centered({100, 50}, 2, 640, 480);
    CHECK(near(cam.x, 100 - 160) && near(cam.y, 50 - 120));
    CHECK(near(cam.center(640, 480), {100, 50}));
    CHECK(near(cam.to_screen({100, 50}), {320, 240}));
    CHECK(near(cam.to_world(cam.to_screen({7, 9})), {7, 9}));
}

void test_smoothing() {
    CameraController c;
    c.set_view_size(640, 480);
    c.snap();
    CHECK(near(c.center(), {0, 0}) && near(c.zoom(), 1));
    c.set_target({100, 0});
    c.update(1.0f / 60);
    CHECK(c.center().x > 0 && c.center().x < 100); // on its way
    CHECK(c.settling());
    settle(c);
    CHECK(near(c.center(), {100, 0}));
    CHECK(!c.settling());

    c.set_target_zoom(4);
    c.update(1.0f / 60);
    CHECK(c.zoom() > 1 && c.zoom() < 4);
    settle(c);
    CHECK(near(c.zoom(), 4));

    // Instant when smoothing is off.
    c.pan_smoothing = 0;
    c.set_target({-50, 20});
    c.update(0.016f);
    CHECK(near(c.center(), {-50, 20}));

    // Clamped zoom.
    c.set_target_zoom(1000);
    CHECK(near(c.target_zoom(), c.max_zoom));
    c.set_target_zoom(0);
    CHECK(near(c.target_zoom(), c.min_zoom));
}

void test_zoom_at_anchor() {
    CameraController c;
    c.set_view_size(800, 600);
    c.set_target({400, 300});
    c.set_target_zoom(1);
    c.snap();
    const Vec2 anchor{100, 500};
    const Vec2 world_before = c.screen_to_world(anchor);
    c.zoom_by(2, anchor);
    settle(c);
    CHECK(near(c.zoom(), 2));
    CHECK(near(c.screen_to_world(anchor), world_before, 0.01f));
    c.zoom_step(-3, anchor);
    settle(c);
    CHECK(near(c.zoom(), 2 / std::pow(1.25f, 3.0f), 1e-3f));
    CHECK(near(c.screen_to_world(anchor), world_before, 0.01f));

    // Pan by screen pixels moves the world the other way, scaled by zoom.
    const Vec2 before = c.target_center();
    c.pan({10, 0});
    CHECK(near(c.target_center().x, before.x - 10 / c.target_zoom()));
}

void test_bounds_and_frame() {
    CameraController c;
    c.set_view_size(400, 300);
    c.set_bounds({0, 0, 1000, 1000});
    c.set_target_zoom(1);
    c.set_target({-500, -500});
    c.snap();
    CHECK(near(c.center(), {200, 150})); // view kept inside the world
    c.set_target({5000, 5000});
    settle(c);
    CHECK(near(c.center(), {800, 850}));
    const Rect v = c.visible_world();
    CHECK(near(v.x, 600) && near(v.y, 700) && near(v.w, 400) && near(v.h, 300));

    // View larger than the bounds: centred on them.
    c.set_target_zoom(0.1f);
    settle(c);
    CHECK(near(c.center(), {500, 500}));

    // frame() picks the zoom that fits.
    c.set_bounds({});
    c.frame({0, 0, 200, 100}, 0);
    settle(c);
    CHECK(near(c.zoom(), 2)); // 400/200 = 2, 300/100 = 3 -> min
    CHECK(near(c.center(), {100, 50}));
    CHECK(c.visible({150, 50, 10, 10}));
    CHECK(!c.visible({500, 500, 10, 10}));
    CHECK(c.visible({205, 50, 10, 10}, 10)); // with margin
}

void test_follow_and_shake() {
    CameraController c;
    c.set_view_size(400, 300);
    c.snap();
    c.follow({10, 0}, 20);
    CHECK(near(c.target_center(), {0, 0})); // inside the dead zone
    c.follow({50, 0}, 20);
    CHECK(near(c.target_center(), {30, 0})); // pulled to the edge of the zone

    c.shake(10, 0.5f);
    c.update(0.05f);
    const Camera shaken = c.camera();
    const Camera calm = Camera::centered(c.center(), c.zoom(), 400, 300);
    CHECK(!near(shaken.x, calm.x, 1e-4f) || !near(shaken.y, calm.y, 1e-4f));
    for (int i = 0; i < 60; ++i) c.update(0.05f);
    const Camera after = c.camera();
    CHECK(near(after.x, Camera::centered(c.center(), c.zoom(), 400, 300).x));
    CHECK(!c.settling());
}

void test_lod() {
    CameraController c;
    c.set_view_size(100, 100);
    c.set_target_zoom(1);
    c.snap();
    CHECK(c.lod() == 0);
    c.set_target_zoom(0.5f);
    c.snap();
    CHECK(c.lod() == 1);
    c.set_target_zoom(0.2f);
    c.snap();
    CHECK(c.lod() == 2);
    CHECK(c.lod(0.2f) == 0);
    c.set_target_zoom(3);
    c.snap();
    CHECK(c.lod() == 0);
    CHECK(near(c.pixels_per_unit(), 3));
}

} // namespace

int main() {
    test_camera_struct();
    test_smoothing();
    test_zoom_at_anchor();
    test_bounds_and_frame();
    test_follow_and_shake();
    test_lod();
    if (failures == 0) std::printf("camera_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
