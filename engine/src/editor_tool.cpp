#include "rewind/debug/editor_tool.h"

#include "rewind/render/network_draw.h"
#include "rewind/render/renderer2d.h"

#include <imgui.h>

namespace rw {

void NetworkTool::pick(const ToolContext& ctx) {
    hover_node_ = kNone;
    hover_edge_ = kNone;
    if (!net_ || !ctx.hovered) return;
    hover_node_ = net_->nearest_node(ctx.mouse_world, ctx.px(pick_px));
    if (hover_node_ != kNone) return;
    float t = 0;
    const u32 e = net_->nearest_edge(ctx.mouse_world, 1e30f, &t);
    if (e != kNone) {
        const float half = net_->edge(e).width * 0.5f + ctx.px(4);
        PathSampler s(net_->edge(e).path, 0.5f);
        if (distance(s.closest(ctx.mouse_world).position, ctx.mouse_world) <= half) {
            hover_edge_ = e;
            hover_t_ = t;
        }
    }
}

u32 NetworkTool::place_node(const ToolContext& ctx, Vec2 at) {
    return net_->add_node(ctx.snap(at), 0, 0, node_radius);
}

void NetworkTool::connect(u32 from, u32 to, bool& edited) {
    if (from == to || !net_->valid_node(from) || !net_->valid_node(to)) return;
    u32 edge = kNone;
    if (smooth) {
        // Leave `from` continuing the direction of its last edge, arrive straight.
        Vec2 dir_a = normalize(net_->node(to).pos - net_->node(from).pos);
        for (u32 e : net_->edges_at(from)) {
            const NetEdge& ne = net_->edge(e);
            if (ne.b == from) dir_a = ne.path.segments.back().direction(1);
            else if (ne.a == from) dir_a = -ne.path.segments.front().direction(0);
        }
        const Vec2 dir_b = normalize(net_->node(to).pos - net_->node(from).pos);
        edge = net_->add_curved_edge(from, to, dir_a, dir_b);
    } else {
        edge = net_->add_edge(from, to);
    }
    if (edge == kNone) return;
    net_->set_tracks(edge, forward_tracks, backward_tracks, track_width);
    edited = true;
}

void NetworkTool::cancel() {
    from_ = kNone;
    press_node_ = kNone;
    dragging_ = false;
    stroke_.clear();
    stroke_from_ = kNone;
}

void NetworkTool::update(ToolContext& ctx) {
    if (!net_) {
        ctx.status = "no network";
        return;
    }
    pick(ctx);
    bool edited = false;

    switch (mode) {
    case Mode::Place:
        if (ctx.pressed && ctx.hovered) {
            press_node_ = hover_node_;
            press_pos_ = ctx.mouse_world;
            dragging_ = false;
        }
        if (ctx.down && press_node_ != kNone) {
            if (!dragging_ && distance(ctx.mouse_world, press_pos_) > ctx.px(4)) dragging_ = true;
            if (dragging_) net_->move_node(press_node_, ctx.snap(ctx.mouse_world));
        }
        if (ctx.released) {
            if (dragging_) {
                edited = true;
                dragging_ = false;
            } else if (ctx.hovered) {
                if (press_node_ != kNone) {
                    if (from_ != kNone && from_ != press_node_) connect(from_, press_node_, edited);
                    from_ = press_node_;
                } else if (hover_edge_ == kNone) {
                    const u32 node = place_node(ctx, ctx.mouse_world);
                    edited = true;
                    if (from_ != kNone) connect(from_, node, edited);
                    from_ = node;
                }
            }
            press_node_ = kNone;
        }
        if (ctx.right_pressed) from_ = kNone;
        ctx.status = from_ == kNone ? "Place: click to add a node, click a node to start a chain, drag to move"
                                    : "Place: click to add the next node, click a node to connect, right-click to end";
        break;
    case Mode::Split:
        if (ctx.released && ctx.hovered && hover_edge_ != kNone) {
            PathSampler s(net_->edge(hover_edge_).path, 0.5f);
            const PathPoint at = s.at(hover_t_ * s.length());
            if (net_->split_edge(hover_edge_, at.segment, at.t) != kNone) edited = true;
        }
        ctx.status = "Split: click an edge to insert a node";
        break;
    case Mode::Delete:
        if (ctx.released && ctx.hovered) {
            if (hover_node_ != kNone) {
                net_->remove_node(hover_node_);
                edited = true;
            } else if (hover_edge_ != kNone) {
                net_->remove_edge(hover_edge_);
                edited = true;
            }
        }
        ctx.status = "Delete: click a node or an edge";
        break;
    case Mode::Draw:
        if (ctx.pressed && ctx.hovered) {
            stroke_.clear();
            stroke_from_ = hover_node_;
            stroke_.push_back(hover_node_ != kNone ? net_->node(hover_node_).pos : ctx.snap(ctx.mouse_world));
        }
        if (ctx.down && !stroke_.empty() && distance(ctx.mouse_world, stroke_.back()) > ctx.px(2)) stroke_.push_back(ctx.mouse_world);
        if (ctx.released && !stroke_.empty()) {
            // The stroke ends where the button came up.
            if (distance(ctx.mouse_world, stroke_.back()) > ctx.px(1)) stroke_.push_back(ctx.mouse_world);
            float drawn = 0;
            for (usize i = 1; i < stroke_.size(); ++i) drawn += distance(stroke_[i], stroke_[i - 1]);
            if (stroke_.size() >= 2 && drawn > ctx.px(8)) {
                const u32 a = stroke_from_ != kNone ? stroke_from_ : place_node(ctx, stroke_.front());
                u32 b = hover_node_ != kNone && hover_node_ != a ? hover_node_ : kNone;
                if (b == kNone) {
                    stroke_.back() = ctx.snap(stroke_.back());
                    b = place_node(ctx, stroke_.back());
                } else {
                    stroke_.back() = net_->node(b).pos;
                }
                Path path = fit_path(simplify_polyline(stroke_, ctx.px(1)), ctx.px(draw_tolerance_px));
                const u32 edge = path.empty() ? kNone : net_->add_edge(a, b, &path);
                if (edge != kNone) {
                    net_->set_tracks(edge, forward_tracks, backward_tracks, track_width);
                    edited = true;
                    from_ = b;
                }
            }
            stroke_.clear();
            stroke_from_ = kNone;
        }
        ctx.status = stroke_.empty() ? "Draw: drag a stroke to add a curved edge; start or end on a node to connect it"
                                     : "Draw: release to fit the stroke";
        break;
    }

    if (edited) {
        net_->rebuild();
        ctx.edited = true;
        if (from_ != kNone && !net_->valid_node(from_)) from_ = kNone;
    }

    // Drawing: the network, the hovered thing, and the chain in progress.
    if (ctx.renderer) {
        Renderer2D& r = *ctx.renderer;
        if (draw_debug) draw_network(r, *net_);
        const float px = ctx.px(1);
        if (hover_node_ != kNone) r.fill_circle(net_->node(hover_node_).pos.x, net_->node(hover_node_).pos.y, 6 * px, rgba(255, 220, 80, 200), 16);
        if (hover_edge_ != kNone) {
            StrokeStyle s;
            s.width = std::max(net_->edge(hover_edge_).width, 2 * px);
            s.r = 1; s.g = 0.86f; s.b = 0.3f; s.a = 0.35f;
            r.draw_path(net_->edge(hover_edge_).path, s);
        }
        if (stroke_.size() >= 2) {
            StrokeStyle s;
            s.width = 2 * px;
            s.join = LineJoin::Round;
            s.r = 1; s.g = 0.86f; s.b = 0.3f; s.a = 0.8f;
            r.draw_polyline(stroke_, s);
        }
        if (from_ != kNone && net_->valid_node(from_) && !dragging_) {
            const Vec2 a = net_->node(from_).pos;
            const Vec2 b = hover_node_ != kNone ? net_->node(hover_node_).pos : ctx.snap(ctx.mouse_world);
            r.draw_line(a.x, a.y, b.x, b.y, rgba(120, 200, 255, 180));
            r.fill_circle(a.x, a.y, 5 * px, rgba(120, 200, 255, 220), 16);
        }
    }
}

void NetworkTool::panel() {
    int m = static_cast<int>(mode);
    const char* modes[] = {"Place", "Split", "Delete", "Draw"};
    if (ImGui::Combo("mode", &m, modes, 4)) mode = static_cast<Mode>(m);
    if (mode == Mode::Draw) ImGui::DragFloat("fit tolerance (px)", &draw_tolerance_px, 0.1f, 0.5f, 20);
    int fwd = static_cast<int>(forward_tracks), back = static_cast<int>(backward_tracks);
    if (ImGui::SliderInt("forward tracks", &fwd, 0, 6)) forward_tracks = static_cast<u32>(fwd);
    if (ImGui::SliderInt("backward tracks", &back, 0, 6)) backward_tracks = static_cast<u32>(back);
    ImGui::DragFloat("track width", &track_width, 0.1f, 0.5f, 64);
    ImGui::DragFloat("node radius", &node_radius, 0.5f, 0, 200);
    ImGui::Checkbox("smooth edges", &smooth);
    ImGui::SameLine();
    ImGui::Checkbox("draw network", &draw_debug);
    ImGui::DragFloat("pick radius (px)", &pick_px, 0.5f, 2, 40);
}

} // namespace rw
