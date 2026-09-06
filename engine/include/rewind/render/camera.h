#pragma once

// A camera controller for 2D views: smooth pan and zoom towards targets,
// zoom around a screen point (mouse wheel), follow with a dead zone, world
// bounds, screen shake, visibility tests and a level-of-detail hint.
//
// This is presentation state, updated with the frame's delta time and not
// part of the simulation. What the camera looks at (a followed entity, a
// zoom level the player chose) can live in reflected game state; the
// controller then just chases it smoothly.

#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/render/renderer2d.h"

namespace rw {

class CameraController {
public:
    // ---- setup --------------------------------------------------------------
    /// Size of the view in render-target pixels. Set once and whenever it changes.
    void set_view_size(float width, float height);
    Vec2 view_size() const { return view_; }

    /// Exponential smoothing rates per second (higher = snappier; 0 = instant).
    float pan_smoothing = 12;
    float zoom_smoothing = 12;
    float min_zoom = 0.05f;
    float max_zoom = 32;
    /// Multiplier per wheel click for zoom_step().
    float zoom_step_factor = 1.25f;

    /// Keep the view inside this world rectangle (w or h <= 0 disables). A
    /// view larger than the bounds centres on them.
    void set_bounds(const Rect& world) { bounds_ = world; }
    const Rect& bounds() const { return bounds_; }

    // ---- targets ------------------------------------------------------------
    void set_target(Vec2 world_center) { target_center_ = world_center; }
    void set_target_zoom(float zoom);
    /// Move the target by a screen-space delta (drag panning).
    void pan(Vec2 screen_delta);
    /// Multiply the target zoom, keeping the world point under `screen_anchor` fixed.
    void zoom_by(float factor, Vec2 screen_anchor);
    void zoom_by(float factor) { zoom_by(factor, view_ * 0.5f); }
    /// `clicks` wheel steps of zoom_step_factor around `screen_anchor`.
    void zoom_step(int clicks, Vec2 screen_anchor);
    /// Chase `world_point`; it may wander `deadzone_radius` from the target
    /// before the camera moves.
    void follow(Vec2 world_point, float deadzone_radius = 0);
    /// Fit a world rectangle into the view with `margin` pixels around it.
    void frame(const Rect& world, float margin = 0);
    /// Jump to the targets now, no smoothing.
    void snap();
    /// Shake with `amplitude` pixels for `seconds`, fading out.
    void shake(float amplitude, float seconds, float frequency = 25);

    // ---- per frame ----------------------------------------------------------
    /// Advance towards the targets. Call once per rendered frame.
    void update(float dt);
    /// update() with the wall-clock time since the previous call (first call = 0).
    void update_realtime();

    // ---- state --------------------------------------------------------------
    Vec2 center() const { return center_; }
    float zoom() const { return zoom_; }
    Vec2 target_center() const { return target_center_; }
    float target_zoom() const { return target_zoom_; }
    /// True while the camera is still noticeably moving towards its targets.
    bool settling() const;

    /// The camera to hand to Renderer2D::set_camera(), shake included.
    Camera camera() const;
    Vec2 world_to_screen(Vec2 world) const { return camera().to_screen(world); }
    Vec2 screen_to_world(Vec2 screen) const { return camera().to_world(screen); }
    /// World rectangle currently on screen.
    Rect visible_world() const;
    /// Whether a world rectangle intersects the view, with `margin` world units of slack.
    bool visible(const Rect& world, float margin = 0) const;

    /// Level of detail: 0 when one world unit covers `base_zoom` pixels or
    /// more, 1 at half that, 2 at a quarter, and so on. Use it to swap
    /// sprites for simpler ones or skip small details when zoomed out.
    int lod(float base_zoom = 1) const;
    float pixels_per_unit() const { return zoom_; }

    void reflect(Visitor& v) {
        v.field("center", center_);
        v.field("zoom", zoom_, {.min = 0.01f, .max = 64});
        v.field("target_center", target_center_);
        v.field("target_zoom", target_zoom_, {.min = 0.01f, .max = 64});
        v.field("pan_smoothing", pan_smoothing, {.min = 0, .max = 60});
        v.field("zoom_smoothing", zoom_smoothing, {.min = 0, .max = 60});
        v.field("min_zoom", min_zoom, {.min = 0.001f, .max = 64});
        v.field("max_zoom", max_zoom, {.min = 0.001f, .max = 256});
        v.field("bounds", bounds_);
    }

private:
    Vec2 clamp_center(Vec2 c, float zoom) const;

    Vec2 view_{1, 1};
    Vec2 center_{};
    float zoom_ = 1;
    Vec2 target_center_{};
    float target_zoom_ = 1;
    Rect bounds_{};
    // Shake.
    float shake_amplitude_ = 0;
    float shake_time_ = 0;
    float shake_duration_ = 0;
    float shake_frequency_ = 25;
    Vec2 shake_offset_{};
    u64 last_realtime_ns_ = 0;
};

} // namespace rw
