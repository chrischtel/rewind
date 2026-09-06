#include "rewind/render/camera.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace rw {

void CameraController::set_view_size(float width, float height) {
    view_ = {std::max(width, 1.0f), std::max(height, 1.0f)};
}

void CameraController::set_target_zoom(float zoom) { target_zoom_ = clampf(zoom, min_zoom, max_zoom); }

void CameraController::pan(Vec2 screen_delta) { target_center_ -= screen_delta / std::max(target_zoom_, 1e-6f); }

void CameraController::zoom_by(float factor, Vec2 screen_anchor) {
    const float before = target_zoom_;
    const float after = clampf(before * factor, min_zoom, max_zoom);
    if (after == before) return;
    // World point under the anchor (at the target state) must stay put.
    const Vec2 offset = screen_anchor - view_ * 0.5f;
    const Vec2 world = target_center_ + offset / before;
    target_zoom_ = after;
    target_center_ = world - offset / after;
}

void CameraController::zoom_step(int clicks, Vec2 screen_anchor) {
    if (clicks == 0) return;
    zoom_by(std::pow(zoom_step_factor, static_cast<float>(clicks)), screen_anchor);
}

void CameraController::follow(Vec2 world_point, float deadzone_radius) {
    const Vec2 d = world_point - target_center_;
    const float dist = length(d);
    if (dist <= deadzone_radius) return;
    target_center_ = world_point - d * (deadzone_radius / std::max(dist, 1e-6f));
}

void CameraController::frame(const Rect& world, float margin) {
    if (world.w <= 0 || world.h <= 0) return;
    const float zx = (view_.x - 2 * margin) / world.w, zy = (view_.y - 2 * margin) / world.h;
    set_target_zoom(std::min(zx, zy));
    target_center_ = {world.x + world.w * 0.5f, world.y + world.h * 0.5f};
}

void CameraController::snap() {
    target_zoom_ = clampf(target_zoom_, min_zoom, max_zoom);
    zoom_ = target_zoom_;
    target_center_ = clamp_center(target_center_, zoom_);
    center_ = target_center_;
}

void CameraController::shake(float amplitude, float seconds, float frequency) {
    shake_amplitude_ = std::max(shake_amplitude_, amplitude);
    shake_duration_ = std::max(seconds, 1e-3f);
    shake_time_ = 0;
    shake_frequency_ = frequency;
}

Vec2 CameraController::clamp_center(Vec2 c, float zoom) const {
    if (bounds_.w <= 0 || bounds_.h <= 0) return c;
    const float half_w = view_.x / (2 * zoom), half_h = view_.y / (2 * zoom);
    if (half_w * 2 >= bounds_.w) c.x = bounds_.x + bounds_.w * 0.5f;
    else c.x = clampf(c.x, bounds_.x + half_w, bounds_.right() - half_w);
    if (half_h * 2 >= bounds_.h) c.y = bounds_.y + bounds_.h * 0.5f;
    else c.y = clampf(c.y, bounds_.y + half_h, bounds_.bottom() - half_h);
    return c;
}

void CameraController::update(float dt) {
    dt = std::max(dt, 0.0f);
    target_zoom_ = clampf(target_zoom_, min_zoom, max_zoom);
    target_center_ = clamp_center(target_center_, target_zoom_);

    // Zoom is smoothed in log space so zooming in and out feel the same.
    const float kz = zoom_smoothing > 0 ? 1 - std::exp(-zoom_smoothing * dt) : 1;
    const float log_now = std::log(zoom_), log_target = std::log(target_zoom_);
    zoom_ = std::exp(log_now + (log_target - log_now) * kz);
    if (absf(zoom_ - target_zoom_) < target_zoom_ * 1e-4f) zoom_ = target_zoom_;

    const float kp = pan_smoothing > 0 ? 1 - std::exp(-pan_smoothing * dt) : 1;
    center_ += (target_center_ - center_) * kp;
    if (length_sq(target_center_ - center_) * zoom_ * zoom_ < 1e-4f) center_ = target_center_;
    center_ = clamp_center(center_, zoom_);

    if (shake_amplitude_ > 0) {
        shake_time_ += dt;
        const float remaining = 1 - shake_time_ / shake_duration_;
        if (remaining <= 0) {
            shake_amplitude_ = 0;
            shake_offset_ = {};
        } else {
            const float a = shake_amplitude_ * remaining * remaining;
            const float t = shake_time_ * shake_frequency_;
            shake_offset_ = {a * std::sin(t * 6.2831853f), a * std::cos(t * 4.9f + 1.3f)};
        }
    }
}

void CameraController::update_realtime() {
    const u64 now = SDL_GetTicksNS();
    const float dt = last_realtime_ns_ == 0 ? 0.0f : std::min(static_cast<float>(now - last_realtime_ns_) / 1e9f, 0.25f);
    last_realtime_ns_ = now;
    update(dt);
}

bool CameraController::settling() const {
    return absf(zoom_ - target_zoom_) > target_zoom_ * 1e-3f || length_sq(target_center_ - center_) * zoom_ * zoom_ > 0.25f ||
           shake_amplitude_ > 0;
}

Camera CameraController::camera() const {
    Camera cam = Camera::centered(center_, zoom_, view_.x, view_.y);
    cam.x += shake_offset_.x / zoom_;
    cam.y += shake_offset_.y / zoom_;
    return cam;
}

Rect CameraController::visible_world() const {
    const Camera cam = camera();
    return {cam.x, cam.y, view_.x / zoom_, view_.y / zoom_};
}

bool CameraController::visible(const Rect& world, float margin) const {
    const Rect v = visible_world();
    return overlaps(Rect{world.x - margin, world.y - margin, world.w + 2 * margin, world.h + 2 * margin}, v);
}

int CameraController::lod(float base_zoom) const {
    if (zoom_ >= base_zoom || zoom_ <= 0) return 0;
    return static_cast<int>(std::floor(std::log2(base_zoom / zoom_) + 1e-4f));
}

} // namespace rw
