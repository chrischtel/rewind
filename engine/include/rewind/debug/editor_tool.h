#pragma once

// Editor tools: the Scene panel's built-in mode selects entities and drags
// their gizmos; a game adds tools of its own (Game::editor_tools) that see
// the mouse in world space, draw into the view, snap through the gizmo
// settings and report edits so undo and the timeline stay right.
//
// NetworkTool is a ready-made one for CurveNetworks: place nodes, connect
// them into edges, drag, split, delete. Any game with a network gets a
// network editor by returning one from editor_tools().

#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/debug/gizmo.h"
#include "rewind/sim/network.h"

#include <string>
#include <vector>

namespace rw {

class Renderer2D;
class World;

struct ToolContext {
    Renderer2D* renderer = nullptr; ///< the game's view, camera set; null in tests
    Gizmos* gizmos = nullptr;       ///< for snapping and handles; null in tests
    World* world = nullptr;
    Vec2 mouse_world;
    Vec2 mouse_render;
    bool hovered = false;   ///< the view is under the mouse
    bool down = false;      ///< left button
    bool pressed = false;
    bool released = false;
    bool right_pressed = false;
    bool shift = false, ctrl = false, alt = false;
    float zoom = 1;
    float dt = 0;
    // Set by the tool:
    bool edited = false;    ///< simulation state changed (undo step, timeline re-snapshot)
    std::string status;     ///< shown in the status bar

    /// World units per pixel at the current zoom.
    float px(float pixels) const { return pixels / (zoom > 0 ? zoom : 1); }
    /// Grid snapping through the gizmo settings, when there are any.
    Vec2 snap(Vec2 p) const { return gizmos ? gizmos->snap_point(p) : p; }
};

class EditorTool {
public:
    virtual ~EditorTool() = default;
    virtual const char* name() const = 0;
    virtual void activate() {}
    virtual void deactivate() {}
    /// Escape, or the tool was switched mid-operation.
    virtual void cancel() {}
    /// Every frame while active, from the scene callback (draw here).
    virtual void update(ToolContext& ctx) = 0;
    /// Options as ImGui widgets, shown by the editor next to the tool picker.
    virtual void panel() {}
};

/// Edits a CurveNetwork in the scene view.
///
/// Place: click empty space for a node (chained to the previous one by an
/// edge), click a node to connect or start from it, drag a node to move
/// it, right-click to end the chain. Split: click an edge. Delete: click a
/// node or edge. Draw: drag a freehand stroke; it is fitted to a curve and
/// becomes an edge, from and to existing nodes when the stroke starts or
/// ends on one. New edges get the tool's track layout.
class NetworkTool final : public EditorTool {
public:
    enum class Mode { Place, Split, Delete, Draw };

    explicit NetworkTool(CurveNetwork* network, const char* name = "Network") : net_(network), name_(name) {}

    const char* name() const override { return name_.c_str(); }
    void update(ToolContext& ctx) override;
    void cancel() override;
    void panel() override;

    void set_network(CurveNetwork* network) { net_ = network; }
    Mode mode = Mode::Place;
    u32 forward_tracks = 1;
    u32 backward_tracks = 1;
    float track_width = 4;
    float node_radius = 0;
    bool smooth = true;          ///< new edges leave the previous node along its last edge
    bool draw_debug = true;      ///< draw_network() overlay
    float pick_px = 10;          ///< hit radius in pixels
    float draw_tolerance_px = 3; ///< Draw: how far the fitted curve may stray from the stroke

    u32 chain_from() const { return from_; }
    const std::vector<Vec2>& stroke() const { return stroke_; }
    u32 hovered_node() const { return hover_node_; }
    u32 hovered_edge() const { return hover_edge_; }

private:
    static constexpr u32 kNone = ~0u;
    void pick(const ToolContext& ctx);
    u32 place_node(const ToolContext& ctx, Vec2 at);
    void connect(u32 from, u32 to, bool& edited);

    CurveNetwork* net_;
    std::string name_;
    u32 from_ = kNone;
    u32 hover_node_ = kNone;
    u32 hover_edge_ = kNone;
    float hover_t_ = 0;
    u32 press_node_ = kNone;
    Vec2 press_pos_;
    bool dragging_ = false;
    std::vector<Vec2> stroke_;
    u32 stroke_from_ = kNone;
};

} // namespace rw
