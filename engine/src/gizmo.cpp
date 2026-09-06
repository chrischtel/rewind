#include "rewind/debug/gizmo.h"

#include <algorithm>
#include <cmath>

namespace rw {

namespace {
constexpr float kPi = 3.14159265f;
constexpr u32 kPartMain = 0;
} // namespace

// ---- frame -------------------------------------------------------------------------

void Gizmos::begin(const GizmoInput& input, const Camera& camera, float view_w, float view_h) {
    prev_in_ = in_;
    in_ = input;
    camera_ = camera;
    view_ = {view_w, view_h};
    mouse_world_ = to_world(in_.mouse);
    shapes_.clear();
    parts_.clear();
    pressed_this_frame_ = in_.down && !prev_in_.down && in_.enabled;
    any_hit_on_press_ = false;
    clicked_empty_ = false;

    if (pressed_this_frame_ && hot_key_ != 0) {
        active_key_ = hot_key_;
        active_id_ = hot_id_;
        press_mouse_ = mouse_world_;
        any_hit_on_press_ = true;
    }
    if (!in_.down && active_key_ != 0) {
        active_key_ = 0;
        active_id_ = 0;
    }
}

void Gizmos::end() {
    // Closest part under the mouse becomes hot for the next frame.
    hot_key_ = 0;
    hot_id_ = 0;
    if (in_.enabled && active_key_ == 0) {
        float best = 1e30f;
        for (const Part& p : parts_) {
            const float d = length(p.screen - in_.mouse);
            if (d <= p.radius_px && d < best) {
                best = d;
                hot_key_ = p.key;
                hot_id_ = static_cast<u32>(p.key >> 8);
            }
        }
    }
    if (pressed_this_frame_ && !any_hit_on_press_) clicked_empty_ = true;
}

// ---- snapping ----------------------------------------------------------------------

Vec2 Gizmos::snap_point(Vec2 world) const {
    if (!snap_.enabled || snap_.grid <= 0 || in_.alt) return world;
    const float g = snap_.grid;
    return {std::round((world.x - snap_.origin.x) / g) * g + snap_.origin.x,
            std::round((world.y - snap_.origin.y) / g) * g + snap_.origin.y};
}

float Gizmos::snap_angle(float radians) const {
    if (!snap_.enabled || snap_.angle_deg <= 0 || in_.alt) return radians;
    const float step = snap_.angle_deg * kPi / 180;
    return std::round(radians / step) * step;
}

// ---- parts -------------------------------------------------------------------------

bool Gizmos::part(u32 id, u32 part_index, Vec2 world, float radius_px, bool* hovered) {
    const u64 k = key(id, part_index);
    parts_.push_back({k, to_screen(world), radius_px, world});
    if (hovered) *hovered = hot_key_ == k;
    if (active_key_ != k) return false;
    if (pressed_this_frame_) {
        press_value_ = world;
        grab_offset_ = world - press_mouse_;
    }
    return true;
}

Vec2 Gizmos::drag_target(Vec2 fallback) const {
    if (active_key_ == 0) return fallback;
    Vec2 target = mouse_world_ + grab_offset_;
    if (in_.shift) {
        // Lock to the axis with the larger movement since the press.
        const Vec2 d = target - press_value_;
        if (absf(d.x) >= absf(d.y)) target.y = press_value_.y;
        else target.x = press_value_.x;
    }
    return snap_point(target);
}

Color Gizmos::color_for(u32 id, u32 part_index, Color base) const {
    const u64 k = key(id, part_index);
    if (active_key_ == k) return style_.active;
    if (hot_key_ == k && active_key_ == 0) return style_.hover;
    return base.a == 0 ? style_.normal : base;
}

void Gizmos::circle(Vec2 world, float radius_px, Color c, bool filled) {
    shapes_.push_back({filled ? Shape::Disc : Shape::Circle, world, {}, radius_px, c, {}});
}
void Gizmos::line(Vec2 a, Vec2 b, Color c) { shapes_.push_back({Shape::Line, a, b, 0, c, {}}); }
void Gizmos::square(Vec2 world, float half_px, Color c) { shapes_.push_back({Shape::Square, world, {}, half_px, c, {}}); }

// ---- handles -----------------------------------------------------------------------

bool Gizmos::point(u32 id, Vec2& world, float radius_px, Color color) {
    if (radius_px <= 0) radius_px = style_.handle_px;
    const bool active = part(id, kPartMain, world, radius_px + 3, nullptr);
    bool changed = false;
    if (active) {
        const Vec2 next = drag_target(world);
        changed = next != world;
        world = next;
    }
    circle(world, radius_px, color_for(id, kPartMain, color), true);
    return changed;
}

bool Gizmos::tangent(u32 id, Vec2 anchor, Vec2& handle) {
    const bool active = part(id, kPartMain, handle, style_.tangent_px + 3, nullptr);
    bool changed = false;
    if (active) {
        const Vec2 next = drag_target(handle);
        changed = next != handle;
        handle = next;
    }
    line(anchor, handle, style_.tangent);
    circle(handle, style_.tangent_px, color_for(id, kPartMain, style_.tangent), false);
    return changed;
}

bool Gizmos::move(u32 id, Vec2& world, float size_px) {
    const float len = size_px / camera_.zoom;
    const Vec2 tip_x = world + Vec2{len, 0};
    const Vec2 tip_y = world - Vec2{0, len}; // up on screen
    bool changed = false;

    // Register all parts against the pre-drag position so hit areas stay put.
    const bool free = part(id, 0, world, style_.handle_px + 4, nullptr);
    const bool along_x = part(id, 1, tip_x, style_.handle_px + 3, nullptr);
    const bool along_y = part(id, 2, tip_y, style_.handle_px + 3, nullptr);
    if (free) {
        const Vec2 next = drag_target(world);
        changed = next != world;
        world = next;
    } else if (along_x || along_y) {
        // The tip was grabbed: the drag target is the tip, the point follows.
        const Vec2 tip_target = drag_target(along_x ? tip_x : tip_y);
        Vec2 next = world;
        if (along_x) next.x = tip_target.x - len;
        else next.y = tip_target.y + len;
        changed = next != world;
        world = next;
    }
    const Vec2 tx = world + Vec2{len, 0}, ty = world - Vec2{0, len};
    line(world, tx, style_.axis_x);
    line(world, ty, style_.axis_y);
    circle(tx, style_.handle_px * 0.8f, color_for(id, 1, style_.axis_x), true);
    circle(ty, style_.handle_px * 0.8f, color_for(id, 2, style_.axis_y), true);
    square(world, style_.handle_px, color_for(id, 0, style_.normal));
    return changed;
}

bool Gizmos::rect(u32 id, Rect& r) {
    // Parts: 0 centre, 1..4 corners (tl tr br bl), 5..8 edges (top right bottom left).
    const Vec2 tl{r.x, r.y}, tr{r.right(), r.y}, br{r.right(), r.bottom()}, bl{r.x, r.bottom()};
    const Vec2 c{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    const Vec2 pts[9] = {c, tl, tr, br, bl, {c.x, r.y}, {r.right(), c.y}, {c.x, r.bottom()}, {r.x, c.y}};
    bool changed = false;
    const Rect before = r;
    for (u32 i = 0; i < 9; ++i) {
        if (!part(id, i, pts[i], (i == 0 ? style_.handle_px + 2 : style_.handle_px) + 3, nullptr)) continue;
        const Vec2 target = drag_target(pts[i]);
        float x0 = before.x, y0 = before.y, x1 = before.right(), y1 = before.bottom();
        switch (i) {
        case 0: {
            const Vec2 d = target - c;
            x0 += d.x; x1 += d.x; y0 += d.y; y1 += d.y;
            break;
        }
        case 1: x0 = target.x; y0 = target.y; break;
        case 2: x1 = target.x; y0 = target.y; break;
        case 3: x1 = target.x; y1 = target.y; break;
        case 4: x0 = target.x; y1 = target.y; break;
        case 5: y0 = target.y; break;
        case 6: x1 = target.x; break;
        case 7: y1 = target.y; break;
        case 8: x0 = target.x; break;
        default: break;
        }
        const Rect next{std::min(x0, x1), std::min(y0, y1), absf(x1 - x0), absf(y1 - y0)};
        changed = next.x != r.x || next.y != r.y || next.w != r.w || next.h != r.h;
        r = next;
    }
    shapes_.push_back({Shape::RectOutline, {r.x, r.y}, {r.right(), r.bottom()}, 0, style_.line, {}});
    const Vec2 ntl{r.x, r.y}, ntr{r.right(), r.y}, nbr{r.right(), r.bottom()}, nbl{r.x, r.bottom()};
    const Vec2 nc{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    const Vec2 npts[9] = {nc, ntl, ntr, nbr, nbl, {nc.x, r.y}, {r.right(), nc.y}, {nc.x, r.bottom()}, {r.x, nc.y}};
    for (u32 i = 1; i < 9; ++i) square(npts[i], i <= 4 ? style_.handle_px * 0.7f : style_.handle_px * 0.5f, color_for(id, i, style_.normal));
    square(nc, style_.handle_px, color_for(id, 0, style_.normal));
    return changed;
}

bool Gizmos::rotate(u32 id, Vec2 center, float& radians, float radius_px) {
    const float rad_world = radius_px / camera_.zoom;
    const Vec2 knob = center + from_angle(radians) * rad_world;
    bool changed = false;
    if (part(id, kPartMain, knob, style_.handle_px + 3, nullptr)) {
        const Vec2 d = mouse_world_ - center;
        if (length_sq(d) > 1e-6f) {
            // Keep the angular offset the user grabbed the knob with.
            static float grab_delta = 0;
            if (pressed_this_frame_) grab_delta = radians - angle_of(d);
            const float next = snap_angle(angle_of(d) + grab_delta);
            changed = next != radians;
            radians = next;
        }
    }
    circle(center, radius_px, style_.line, false);
    const Vec2 nk = center + from_angle(radians) * rad_world;
    line(center, nk, style_.line);
    circle(nk, style_.handle_px, color_for(id, kPartMain, style_.normal), true);
    return changed;
}

bool Gizmos::path(u32 id, std::vector<Vec2>& cp, bool closed, bool mirror, bool draw_curve) {
    const usize segs = cp.size() / 4;
    if (segs == 0) return false;
    bool changed = false;
    const std::vector<Vec2> before = cp;

    // Joints: anchor i is cp[4i] (start of segment i) shared with cp[4(i-1)+3].
    auto prev_seg = [&](usize s) -> std::ptrdiff_t {
        if (s > 0) return static_cast<std::ptrdiff_t>(s - 1);
        return closed ? static_cast<std::ptrdiff_t>(segs - 1) : -1;
    };
    auto next_seg = [&](usize s) -> std::ptrdiff_t {
        if (s + 1 < segs) return static_cast<std::ptrdiff_t>(s + 1);
        return closed ? 0 : -1;
    };

    // Anchors: one per segment start, plus the free end of an open path.
    // Part indices: anchor of segment s = s * 3 + 0, its outgoing tangent = s*3+1, incoming tangent (cp[4s+2]) = s*3+2.
    for (usize s = 0; s < segs; ++s) {
        const u32 base = static_cast<u32>(s * 3);
        Vec2 anchor = before[4 * s];
        if (part(id, base + 0, anchor, style_.handle_px + 3, nullptr)) {
            const Vec2 next = drag_target(anchor);
            const Vec2 delta = next - anchor;
            if (delta != Vec2{}) {
                changed = true;
                cp[4 * s] = next;
                cp[4 * s + 1] += delta;
                const std::ptrdiff_t ps = prev_seg(s);
                if (ps >= 0) {
                    cp[4 * static_cast<usize>(ps) + 3] = next;
                    cp[4 * static_cast<usize>(ps) + 2] += delta;
                }
            }
        }
        // Outgoing tangent cp[4s+1], mirrored against the previous segment's cp[4ps+2].
        Vec2 t_out = before[4 * s + 1];
        if (part(id, base + 1, t_out, style_.tangent_px + 3, nullptr)) {
            const Vec2 next = drag_target(t_out);
            if (next != t_out) {
                changed = true;
                cp[4 * s + 1] = next;
                const std::ptrdiff_t ps = prev_seg(s);
                if (mirror && ps >= 0) {
                    const Vec2 a = cp[4 * s];
                    Vec2& opposite = cp[4 * static_cast<usize>(ps) + 2];
                    const float len = length(opposite - a);
                    opposite = a - normalize(next - a) * len;
                }
            }
        }
        // Incoming tangent cp[4s+2], mirrored against the next segment's cp[4ns+1].
        Vec2 t_in = before[4 * s + 2];
        if (part(id, base + 2, t_in, style_.tangent_px + 3, nullptr)) {
            const Vec2 next = drag_target(t_in);
            if (next != t_in) {
                changed = true;
                cp[4 * s + 2] = next;
                const std::ptrdiff_t ns = next_seg(s);
                if (mirror && ns >= 0) {
                    const Vec2 a = cp[4 * s + 3];
                    Vec2& opposite = cp[4 * static_cast<usize>(ns) + 1];
                    const float len = length(opposite - a);
                    opposite = a - normalize(next - a) * len;
                }
            }
        }
    }
    // Free end of an open path.
    if (!closed) {
        const u32 end_part = static_cast<u32>(segs * 3);
        Vec2 anchor = before[4 * (segs - 1) + 3];
        if (part(id, end_part, anchor, style_.handle_px + 3, nullptr)) {
            const Vec2 next = drag_target(anchor);
            const Vec2 delta = next - anchor;
            if (delta != Vec2{}) {
                changed = true;
                cp[4 * (segs - 1) + 3] = next;
                cp[4 * (segs - 1) + 2] += delta;
            }
        }
    }

    // Draw: curve, stems, tangents, anchors.
    if (draw_curve) {
        Shape curve{Shape::Polyline, {}, {}, 0, style_.curve, {}};
        for (usize s = 0; s < segs; ++s) {
            const CubicBezier b{cp[4 * s], cp[4 * s + 1], cp[4 * s + 2], cp[4 * s + 3]};
            const int steps = 16;
            for (int i = (s == 0 ? 0 : 1); i <= steps; ++i) curve.points.push_back(b.point(static_cast<float>(i) / steps));
        }
        shapes_.push_back(std::move(curve));
    }
    for (usize s = 0; s < segs; ++s) {
        const u32 base = static_cast<u32>(s * 3);
        line(cp[4 * s], cp[4 * s + 1], style_.tangent);
        line(cp[4 * s + 3], cp[4 * s + 2], style_.tangent);
        circle(cp[4 * s + 1], style_.tangent_px, color_for(id, base + 1, style_.tangent), false);
        circle(cp[4 * s + 2], style_.tangent_px, color_for(id, base + 2, style_.tangent), false);
        circle(cp[4 * s], style_.handle_px, color_for(id, base + 0, style_.normal), true);
    }
    if (!closed) circle(cp[4 * (segs - 1) + 3], style_.handle_px, color_for(id, static_cast<u32>(segs * 3), style_.normal), true);
    return changed;
}

bool Gizmos::path(u32 id, Path& p, bool mirror) {
    std::vector<Vec2> cp;
    cp.reserve(p.segments.size() * 4);
    for (const CubicBezier& s : p.segments) {
        cp.push_back(s.p0);
        cp.push_back(s.p1);
        cp.push_back(s.p2);
        cp.push_back(s.p3);
    }
    if (!path(id, cp, p.closed, mirror)) return false;
    for (usize s = 0; s < p.segments.size(); ++s)
        p.segments[s] = {cp[4 * s], cp[4 * s + 1], cp[4 * s + 2], cp[4 * s + 3]};
    return true;
}

// ---- drawing -----------------------------------------------------------------------

void Gizmos::draw(Renderer2D& r) const {
    const float zoom = std::max(r.camera().zoom, 1e-6f);
    const float px = 1.0f / zoom;

    if (snap_.enabled && snap_.grid > 0) {
        const Rect vis{camera_.x, camera_.y, view_.x / zoom, view_.y / zoom};
        const float g = snap_.grid;
        if (vis.w / g <= 200 && vis.h / g <= 200) {
            const float x0 = std::floor((vis.x - snap_.origin.x) / g) * g + snap_.origin.x;
            const float y0 = std::floor((vis.y - snap_.origin.y) / g) * g + snap_.origin.y;
            for (float x = x0; x <= vis.right(); x += g) r.draw_line(x, vis.y, x, vis.bottom(), style_.grid);
            for (float y = y0; y <= vis.bottom(); y += g) r.draw_line(vis.x, y, vis.right(), y, style_.grid);
        }
    }

    for (const Shape& s : shapes_) {
        switch (s.kind) {
        case Shape::Disc: r.fill_circle(s.a.x, s.a.y, s.size_px * px, s.color, 16); break;
        case Shape::Circle: {
            StrokeStyle st;
            st.width = 1.5f * px;
            st.closed = true;
            st.r = s.color.r / 255.0f; st.g = s.color.g / 255.0f; st.b = s.color.b / 255.0f; st.a = s.color.a / 255.0f;
            std::vector<Vec2> ring;
            for (int i = 0; i < 20; ++i) ring.push_back(s.a + from_angle(static_cast<float>(i) / 20 * 2 * kPi) * (s.size_px * px));
            r.draw_polyline(ring, st);
            break;
        }
        case Shape::Line: r.draw_line(s.a.x, s.a.y, s.b.x, s.b.y, s.color); break;
        case Shape::Square: {
            const float h = s.size_px * px;
            r.fill_rect(s.a.x - h, s.a.y - h, 2 * h, 2 * h, s.color);
            break;
        }
        case Shape::RectOutline: r.draw_rect(s.a.x, s.a.y, s.b.x - s.a.x, s.b.y - s.a.y, s.color); break;
        case Shape::Polyline: {
            StrokeStyle st;
            st.width = 1.5f * px;
            st.r = s.color.r / 255.0f; st.g = s.color.g / 255.0f; st.b = s.color.b / 255.0f; st.a = s.color.a / 255.0f;
            r.draw_polyline(s.points, st);
            break;
        }
        }
    }
}

// ---- GizmoVisitor ------------------------------------------------------------------

void GizmoVisitor::visit(std::string_view, bool& v, const FieldMeta&) {
    if (collecting() && frames_.back().widget == Widget::Path && !frames_.back().closed) frames_.back().closed = &v;
}

void GizmoVisitor::visit(std::string_view, float& v, const FieldMeta&) {
    if (collecting()) frames_.back().floats.push_back(&v);
}

bool GizmoVisitor::begin_struct(std::string_view n, const FieldMeta& m) {
    Frame f;
    f.label = std::string(n);
    f.read_only = m.read_only;
    if (collecting()) f.widget = frames_.back().widget;
    frames_.push_back(std::move(f));
    return true;
}

void GizmoVisitor::hint(Widget widget) {
    if (frames_.empty() || widget == Widget::Default) return;
    Frame& f = frames_.back();
    if (f.widget == Widget::Default && (widget == Widget::Vec2 || widget == Widget::Rect || widget == Widget::Path))
        f.widget = widget;
}

void GizmoVisitor::end_struct() {
    if (frames_.empty()) return;
    Frame f = std::move(frames_.back());
    frames_.pop_back();
    if (f.widget == Widget::Default) return;
    if (!frames_.empty() && frames_.back().widget != Widget::Default) {
        Frame& parent = frames_.back();
        parent.floats.insert(parent.floats.end(), f.floats.begin(), f.floats.end());
        if (!parent.closed) parent.closed = f.closed;
        return;
    }
    Target t;
    t.kind = f.widget;
    t.label = f.label;
    t.floats = std::move(f.floats);
    t.closed = f.closed;
    t.read_only = f.read_only;
    for (const Frame& outer : frames_) t.read_only = t.read_only || outer.read_only;
    targets_.push_back(std::move(t));
}

bool GizmoVisitor::begin_array(std::string_view, usize&, const FieldMeta&) {
    return collecting(); // arrays only matter inside a Path
}

bool GizmoVisitor::apply(Gizmos& g, u32 id_base, bool move_gizmo) const {
    bool changed = false;
    u32 id = id_base;
    for (const Target& t : targets_) {
        ++id;
        if (t.read_only) continue;
        switch (t.kind) {
        case Widget::Vec2:
            if (t.floats.size() >= 2) {
                Vec2 v{*t.floats[0], *t.floats[1]};
                if (move_gizmo ? g.move(id, v) : g.point(id, v)) {
                    *t.floats[0] = v.x;
                    *t.floats[1] = v.y;
                    changed = true;
                }
            }
            break;
        case Widget::Rect:
            if (t.floats.size() >= 4) {
                Rect r{*t.floats[0], *t.floats[1], *t.floats[2], *t.floats[3]};
                if (g.rect(id, r)) {
                    *t.floats[0] = r.x;
                    *t.floats[1] = r.y;
                    *t.floats[2] = r.w;
                    *t.floats[3] = r.h;
                    changed = true;
                }
            }
            break;
        case Widget::Path: {
            const usize n = t.floats.size() / 2;
            std::vector<Vec2> cp(n);
            for (usize i = 0; i < n; ++i) cp[i] = {*t.floats[2 * i], *t.floats[2 * i + 1]};
            if (g.path(id, cp, t.closed ? *t.closed : false)) {
                for (usize i = 0; i < n; ++i) {
                    *t.floats[2 * i] = cp[i].x;
                    *t.floats[2 * i + 1] = cp[i].y;
                }
                changed = true;
            }
            break;
        }
        default: break;
        }
    }
    return changed;
}

} // namespace rw
