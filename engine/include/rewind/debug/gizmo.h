#pragma once

// Immediate-mode gizmos: draggable handles in world space for tools and
// debug views. Call begin() with the frame's mouse state and camera, ask
// for handles (each returns whether its value changed), end(), then draw().
//
// Interaction is separated from drawing: handles record shapes, draw()
// pushes them through a Renderer2D. That keeps the logic testable without
// a GPU and lets a host draw gizmos wherever it wants.
//
// Ids are yours: any stable number per handle (an entity index, a hash of
// a field name). Snapping applies to every handle while enabled; holding
// Alt bypasses it, Shift constrains free moves to one axis.

#include "rewind/core/curve.h"
#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/render/renderer2d.h"
#include "rewind/sim/reflect.h"

#include <string>
#include <vector>

namespace rw {

struct GizmoInput {
    Vec2 mouse;         ///< render-target pixels
    bool down = false;  ///< left button
    bool alt = false;   ///< bypass snapping
    bool shift = false; ///< constrain to an axis
    bool enabled = true; ///< false: nothing is hot or activated (view not hovered), drags in progress still finish
};

struct GizmoSnap {
    bool enabled = false;
    float grid = 8;        ///< world units; 0 = no position snapping
    float angle_deg = 15;  ///< 0 = no angle snapping
    Vec2 origin;           ///< grid origin
};

struct GizmoStyle {
    float handle_px = 6;
    float tangent_px = 4;
    Color normal = rgb(255, 255, 255);
    Color hover = rgb(255, 220, 80);
    Color active = rgb(255, 140, 40);
    Color line = rgba(255, 255, 255, 150);
    Color tangent = rgba(120, 200, 255, 200);
    Color curve = rgb(120, 200, 255);
    Color axis_x = rgb(240, 80, 80);
    Color axis_y = rgb(80, 220, 80);
    Color grid = rgba(255, 255, 255, 18);
};

class Gizmos {
public:
    /// Start a frame. `view_w/h` (render pixels) size the snap grid overlay.
    void begin(const GizmoInput& input, const Camera& camera, float view_w, float view_h);
    /// Finish the frame: resolves hover for the next frame and releases drags.
    void end();

    GizmoSnap& snap() { return snap_; }
    const GizmoSnap& snap() const { return snap_; }
    GizmoStyle& style() { return style_; }

    // ---- handles (all return true when the value changed this frame) -------------
    /// A round grab point.
    bool point(u32 id, Vec2& world, float radius_px = 0, Color color = {0, 0, 0, 0});
    /// A smaller handle on a stem from `anchor`.
    bool tangent(u32 id, Vec2 anchor, Vec2& handle);
    /// Translate: centre square moves freely, the two arrow tips move along one axis.
    bool move(u32 id, Vec2& world, float size_px = 48);
    /// Corners resize two edges, edge midpoints one, the centre moves the whole rectangle.
    bool rect(u32 id, Rect& r);
    /// Ring around `center`; dragging on it turns `radians` (snaps to snap().angle_deg).
    bool rotate(u32 id, Vec2 center, float& radians, float radius_px = 48);
    /// Cubic Bezier control points, 4 per segment (p0 p1 p2 p3, p3 == next p0).
    /// Anchors drag with their tangents; with `mirror`, moving a tangent turns
    /// the opposite one to stay aligned across the joint.
    bool path(u32 id, std::vector<Vec2>& control_points, bool closed, bool mirror = true, bool draw_curve = true);
    bool path(u32 id, Path& p, bool mirror = true);

    // ---- state ---------------------------------------------------------------
    /// Handle under the mouse (id given to the handle), 0 for none.
    u32 hot() const { return hot_id_; }
    /// Handle being dragged, 0 for none.
    u32 active() const { return active_id_; }
    bool dragging() const { return active_id_ != 0; }
    /// Whether the last frame's mouse press hit nothing (a click on empty space).
    bool clicked_empty() const { return clicked_empty_; }

    Vec2 snap_point(Vec2 world) const;
    float snap_angle(float radians) const;
    Vec2 mouse_world() const { return mouse_world_; }

    // ---- drawing -------------------------------------------------------------
    struct Shape {
        enum Kind : u8 { Circle, Disc, Line, Square, RectOutline, Polyline } kind;
        Vec2 a, b;        ///< world points (line ends, rect min/max) or centre
        float size_px;    ///< radius or half size, in pixels
        Color color;
        std::vector<Vec2> points; ///< Polyline only
    };
    const std::vector<Shape>& shapes() const { return shapes_; }
    /// Push the recorded shapes (and the snap grid) through the renderer,
    /// which must have the camera given to begin().
    void draw(Renderer2D& r) const;

private:
    struct Part {
        u64 key;
        Vec2 screen;
        float radius_px;
        Vec2 world;
    };
    u64 key(u32 id, u32 part) const { return (static_cast<u64>(id) << 8) | part; }
    /// Register a hit area; returns true while this part is being dragged.
    bool part(u32 id, u32 part_index, Vec2 world, float radius_px, bool* hovered);
    Vec2 to_screen(Vec2 world) const { return camera_.to_screen(world); }
    Vec2 to_world(Vec2 screen) const { return camera_.to_world(screen); }
    Color color_for(u32 id, u32 part_index, Color base) const;
    void circle(Vec2 world, float radius_px, Color c, bool filled);
    void line(Vec2 a, Vec2 b, Color c);
    void square(Vec2 world, float half_px, Color c);
    /// Drag position for the active part: mouse minus grab offset, snapped, axis-constrained.
    Vec2 drag_target(Vec2 fallback) const;

    GizmoInput in_;
    GizmoInput prev_in_;
    Camera camera_;
    Vec2 view_{};
    Vec2 mouse_world_{};
    GizmoSnap snap_;
    GizmoStyle style_;
    std::vector<Shape> shapes_;

    // Hover is resolved at end() from this frame's parts and applies next frame.
    std::vector<Part> parts_;
    u64 hot_key_ = 0;
    u32 hot_id_ = 0;
    u64 active_key_ = 0;
    u32 active_id_ = 0;
    Vec2 grab_offset_{};    ///< value - mouse_world at press
    Vec2 press_value_{};
    Vec2 press_mouse_{};
    bool pressed_this_frame_ = false;
    bool clicked_empty_ = false;
    bool any_hit_on_press_ = false;
};

/// Collects gizmo-able fields from reflected state: every struct hinted
/// Vec2, Rect or Path becomes a handle description with pointers into the
/// data. Tools walk the result, run the matching gizmo, and write back.
class GizmoVisitor final : public Visitor {
public:
    struct Target {
        Widget kind;
        std::string label;
        std::vector<float*> floats; ///< Vec2: x y; Rect: x y w h; Path: 8 per segment
        bool* closed = nullptr;     ///< Path only
        bool read_only = false;
    };
    const std::vector<Target>& targets() const { return targets_; }

    /// Run every collected target through `g`. Returns true when anything changed.
    bool apply(Gizmos& g, u32 id_base, bool move_gizmo = true) const;

    void visit(std::string_view n, bool& v, const FieldMeta& m) override;
    void visit(std::string_view, i8&, const FieldMeta&) override {}
    void visit(std::string_view, i16&, const FieldMeta&) override {}
    void visit(std::string_view, i32&, const FieldMeta&) override {}
    void visit(std::string_view, i64&, const FieldMeta&) override {}
    void visit(std::string_view, u8&, const FieldMeta&) override {}
    void visit(std::string_view, u16&, const FieldMeta&) override {}
    void visit(std::string_view, u32&, const FieldMeta&) override {}
    void visit(std::string_view, u64&, const FieldMeta&) override {}
    void visit(std::string_view n, float& v, const FieldMeta& m) override;
    void visit(std::string_view, double&, const FieldMeta&) override {}
    void visit(std::string_view, std::string&, const FieldMeta&) override {}
    bool begin_struct(std::string_view n, const FieldMeta& m) override;
    void end_struct() override;
    bool begin_array(std::string_view n, usize& count, const FieldMeta& m) override;
    void end_array() override {}
    bool begin_element(usize) override { return true; }
    void end_element() override {}
    void hint(Widget widget) override;

private:
    struct Frame {
        Widget widget = Widget::Default;
        std::string label;
        bool read_only = false;
        std::vector<float*> floats;
        bool* closed = nullptr;
    };
    bool collecting() const { return !frames_.empty() && frames_.back().widget != Widget::Default; }
    std::vector<Frame> frames_;
    std::vector<Target> targets_;
};

} // namespace rw
