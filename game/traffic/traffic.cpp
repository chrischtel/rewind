// Traffic: a top-down traffic management sim on the Rewind engine.
//
// Drag with the left mouse button to draw a road. The stroke is fitted to a
// curve, becomes an edge of the road network with the lanes of the selected
// road type, and connects to a junction when it starts or ends on one. Cars
// appear, plan a route over the network, follow the car in front, queue at
// junctions and give way to whoever is already crossing.
//
// Everything the game simulates -- the network, the cars, the build tool,
// even the camera -- is reflected state on the fixed step. So the whole city
// rewinds with F4, replays with F5, and the inspector can edit any of it.
//
// Units are metres and seconds: 3.5 m lanes, 4.4 m cars, 25 m/s = 90 km/h.

#include <rewind/rewind.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace rw;

namespace {

/// The window is a fixed size on purpose: the simulation turns recorded
/// mouse pixels into world positions itself, so a replay has to see the
/// same view it was recorded with.
constexpr float kWidth = 1440;
constexpr float kHeight = 810;
constexpr u32 kNone = ~0u;

const Color kGround = rgb(24, 32, 27);
const Color kAsphalt = rgb(62, 63, 70);
const Color kKerb = rgba(150, 152, 162, 220);
const Color kCentreLine = rgb(224, 206, 122);
const Color kLaneLine = rgba(228, 228, 234, 130);
const Color kGhost = rgba(255, 214, 90, 200);
const Color kBad = rgba(255, 110, 110, 220);

const Color kCarColors[8] = {rgb(216, 96, 96),  rgb(96, 152, 216),  rgb(228, 200, 116), rgb(118, 200, 150),
                             rgb(206, 206, 214), rgb(146, 120, 206), rgb(232, 152, 88),  rgb(96, 100, 112)};

/// Commands from the HUD. They ride in the input frame, so a click on a
/// button is recorded, rewinds and replays like a key press.
enum Command : u32 { kCmdTool = 1, kCmdRoad = 2, kCmdClearCars = 3 };
enum Event : u32 { kEvBuilt = 1, kEvRemoved = 2 };

constexpr u32 kToolBuild = 0;
constexpr u32 kToolBulldoze = 1;
const char* const kToolNames[] = {"build", "bulldoze"};

// HUD panels sit in the top-left corner, so their rectangles do not depend
// on the view size and the simulation can ignore clicks that land on them.
const Rect kToolPanel{12, 12, 210, 258};
const Rect kInfoPanel{12, 282, 210, 188};
const Rect kHudRect{0, 0, 236, 482};

} // namespace

/// A kind of vehicle, from assets/vehicles.json.
struct VehicleType {
    std::string id;
    float length = 4.4f;
    float width = 1.8f;
    float top_speed_kmh = 90;
    float accel = 3.2f;  ///< m/s²
    float decel = 6.0f;  ///< comfortable braking
    float share = 1;     ///< relative share of the traffic
    u32 color = 0;       ///< index into the car palette

    void reflect(Visitor& v) {
        v.field("id", id);
        v.field("length", length, {.min = 1, .max = 30});
        v.field("width", width, {.min = 0.5f, .max = 4});
        v.field("top_speed_kmh", top_speed_kmh, {.min = 5, .max = 250});
        v.field("accel", accel, {.min = 0.2f, .max = 20});
        v.field("decel", decel, {.min = 0.5f, .max = 20});
        v.field("share", share, {.min = 0, .max = 100});
        v.field("color", color, {.min = 0, .max = 7});
    }
};

/// A kind of road, from assets/road_types.json.
struct RoadType {
    std::string id;
    u32 lanes_forward = 1;
    u32 lanes_backward = 1;
    float lane_width = 3.5f;
    float speed_kmh = 50;

    float width() const { return static_cast<float>(lanes_forward + lanes_backward) * lane_width; }

    void reflect(Visitor& v) {
        v.field("id", id);
        v.field("lanes_forward", lanes_forward, {.min = 0, .max = 5});
        v.field("lanes_backward", lanes_backward, {.min = 0, .max = 5});
        v.field("lane_width", lane_width, {.min = 1.5f, .max = 8});
        v.field("speed_kmh", speed_kmh, {.min = 10, .max = 160});
    }
};

/// What the game knows about a car beyond what the agent itself carries.
/// Indexed like the agents, so it survives in the saved state.
struct CarInfo {
    u32 type = 0;
    u32 dest_edge = 0; ///< a TrackRef: edge indices stay valid across rebuilds
    u32 dest_track = 0;

    void reflect(Visitor& v) {
        v.field("type", type);
        v.field("dest_edge", dest_edge);
        v.field("dest_track", dest_track);
    }
};

class Traffic final : public Game {
public:
    void configure(AppConfig& cfg) override {
        cfg.title = "Traffic";
        cfg.organization = "Rewind";
        cfg.width = static_cast<int>(kWidth);
        cfg.height = static_cast<int>(kHeight);
        cfg.resizable = false; // see kWidth: recorded clicks must mean the same world point
        cfg.timeline.tick_rate = 60;
        cfg.timeline.history_ticks = 60 * 90;
        cfg.timeline.keyframe_interval = 10; // a whole city per tick would be a lot of memory

        // Handy for screenshots and scripted runs: traffic --zoom 6 --spawn 0.3
        for (usize i = 0; i + 1 < cfg.args.size(); ++i) {
            if (cfg.args[i] == "--zoom") start_zoom_ = clampf(std::stof(cfg.args[i + 1]), 1.0f, 24.0f);
            if (cfg.args[i] == "--spawn") start_spawn_ = clampf(std::stof(cfg.args[i + 1]), 0.05f, 10.0f);
        }
    }

    void start(App& app) override {
        app_ = &app;
        std::string error;
        if (!vehicles_.load(&error)) {
            RW_LOG_WARN("traffic", "%s", error.c_str());
            vehicles_.add_row("car");
        }
        if (!roads_.load(&error)) {
            RW_LOG_WARN("traffic", "%s", error.c_str());
            roads_.add_row("street");
        }
    }

    std::vector<DataTableBase*> data_tables() override { return {&vehicles_, &roads_}; }
    /// The editor gets the engine's road tool for the same network.
    std::vector<EditorTool*> editor_tools() override { return {&net_tool_}; }

    void init() override {
        net_.clear();
        cars_.clear();
        car_info_.clear();
        rng_ = Rng(20260907);
        spawned_ = arrived_ = 0;
        spawn_accum_ = 0;
        drawing_ = false;
        stroke_.clear();
        stroke_from_ = kNone;
        tool_ = kToolBuild;
        road_type_ = DefRef<RoadType>{roads_.size() ? roads_.id_at(0) : std::string("street")};
        build_starting_grid();
        cam_target_ = cam_center_ = {210, 210};
        zoom_target_ = zoom_ = start_zoom_; // 1.7: the whole starting grid fits
        spawn_interval_ = start_spawn_;
    }

    // ---- simulation ---------------------------------------------------------

    void tick(const TickContext& ctx) override {
        handle_command(ctx.input);
        update_camera(ctx.input, ctx.dt);
        update_tool(ctx);
        prev_input_ = ctx.input;

        for (u32 i = 0; i < static_cast<u32>(cars_.agents().size()); ++i) {
            if (cars_.valid(i) && cars_.agent(i).done) {
                cars_.remove(i);
                ++arrived_;
            }
        }
        spawn_accum_ += ctx.dt;
        while (spawn_accum_ >= spawn_interval_) {
            spawn_accum_ -= spawn_interval_;
            spawn_car();
        }
        cars_.tick(net_, ctx.dt);
    }

    void on_event(const SimEvent& e, Audio&) override {
        if (e.id == kEvBuilt) dust_.emit(build_puff_, {e.a, e.b}, 18);
        if (e.id == kEvRemoved) dust_.emit(rubble_, {e.a, e.b}, 22);
    }

    void reflect(Visitor& v) override {
        v.field("network", net_);
        v.field("cars", cars_);
        v.field("car_info", car_info_);
        v.field("rng", rng_);
        v.field("tool", tool_, {.names = kToolNames, .name_count = 2});
        v.field("road_type", road_type_, {.table = "road_types"});
        v.field("stroke", stroke_);
        v.field("stroke_from", stroke_from_);
        v.field("drawing", drawing_);
        v.field("cam_center", cam_center_);
        v.field("cam_target", cam_target_);
        v.field("zoom", zoom_, {.min = 1, .max = 40});
        v.field("zoom_target", zoom_target_, {.min = 1, .max = 40});
        v.field("spawn_accum", spawn_accum_);
        v.field("spawn_interval", spawn_interval_, {.min = 0.05f, .max = 10, .speed = 0.05f});
        v.field("max_cars", max_cars_, {.min = 0, .max = 4000});
        v.field("spawned", spawned_, {.read_only = true});
        v.field("arrived", arrived_, {.read_only = true});
        v.field("prev_input", prev_input_);
    }

    // ---- rendering ----------------------------------------------------------

    void render(Renderer2D& r, float /*alpha*/) override {
        r.clear(kGround);
        r.set_camera(Camera::centered(cam_center_, zoom_, kWidth, kHeight));

        draw_roads(r);
        draw_cars(r);
        draw_tool(r);

        dust_.update(app_ ? app_->stats().frame_ms / 1000.0f : 1.0f / 60);
        dust_.draw(r);

        draw_hud(r);
    }

    void debug_ui() override {
        ImGui::Text("%zu cars, %zu edges, %zu tracks", cars_.count(), live_edges(), net_.track_count());
        ImGui::Text("%u spawned, %u arrived", spawned_, arrived_);
        if (ImGui::Button("clear traffic")) clear_traffic();
    }

private:
    // ---- input --------------------------------------------------------------

    Camera view(Vec2 center, float zoom) const { return Camera::centered(center, zoom, kWidth, kHeight); }
    Vec2 to_world(const InputFrame& in) const {
        return view(cam_center_, zoom_).to_world({static_cast<float>(in.mouse_x), static_cast<float>(in.mouse_y)});
    }
    /// Pixels converted to world units at the current zoom.
    float px(float pixels) const { return pixels / (zoom_ > 0 ? zoom_ : 1); }

    void handle_command(const InputFrame& in) {
        switch (in.command) {
        case kCmdTool: tool_ = static_cast<u32>(in.command_value) <= kToolBulldoze ? static_cast<u32>(in.command_value) : tool_; break;
        case kCmdRoad:
            if (in.command_value >= 0 && static_cast<usize>(in.command_value) < roads_.size())
                road_type_ = DefRef<RoadType>{roads_.id_at(static_cast<usize>(in.command_value))};
            break;
        case kCmdClearCars: clear_traffic(); break;
        default: break;
        }
    }

    void update_camera(const InputFrame& in, float dt) {
        const float pan_speed = 900.0f / zoom_target_;
        cam_target_.x += static_cast<float>(in.axis_x) / 32767.0f * pan_speed * dt;
        cam_target_.y += static_cast<float>(in.axis_y) / 32767.0f * pan_speed * dt;

        if (in.wheel != 0) {
            // Zoom around the cursor: the world point under it stays put.
            const Vec2 mouse{static_cast<float>(in.mouse_x), static_cast<float>(in.mouse_y)};
            const Vec2 before = view(cam_target_, zoom_target_).to_world(mouse);
            zoom_target_ = clampf(zoom_target_ * std::pow(1.2f, static_cast<float>(in.wheel)), 1.0f, 24.0f);
            const Vec2 after = view(cam_target_, zoom_target_).to_world(mouse);
            cam_target_ = cam_target_ + (before - after);
        }
        if (in.down(Button::MouseRight) && prev_input_.down(Button::MouseRight)) {
            cam_target_.x -= static_cast<float>(in.mouse_x - prev_input_.mouse_x) / zoom_target_;
            cam_target_.y -= static_cast<float>(in.mouse_y - prev_input_.mouse_y) / zoom_target_;
        }
        // Plain arithmetic smoothing on the fixed step: the view is part of
        // the simulation, so rewinding gives back the camera motion too.
        const float k = clampf(12.0f * dt, 0, 1);
        cam_center_ = cam_center_ + (cam_target_ - cam_center_) * k;
        zoom_ += (zoom_target_ - zoom_) * k;
    }

    void update_tool(const TickContext& ctx) {
        const InputFrame& in = ctx.input;
        const Vec2 world = to_world(in);
        const bool on_hud = in.mouse_x < 0 || kHudRect.contains(static_cast<float>(in.mouse_x), static_cast<float>(in.mouse_y));
        const bool press = in.down(Button::MouseLeft) && !prev_input_.down(Button::MouseLeft);
        const bool release = !in.down(Button::MouseLeft) && prev_input_.down(Button::MouseLeft);
        const bool cancel = in.down(Button::MouseRight) && !prev_input_.down(Button::MouseRight);

        hover_node_ = net_.nearest_node(world, snap_range());
        hover_edge_ = kNone;
        if (hover_node_ == kNone) {
            const u32 e = net_.nearest_edge(world, 1e30f);
            if (e != kNone && distance(closest_on_edge(e, world), world) <= net_.edge(e).width * 0.5f + px(3)) hover_edge_ = e;
        }

        if (tool_ == kToolBuild) {
            if (drawing_ && cancel) {
                drawing_ = false;
                stroke_.clear();
                stroke_from_ = kNone;
            } else if (press && !on_hud) {
                drawing_ = true;
                stroke_.clear();
                stroke_from_ = hover_node_;
                stroke_.push_back(hover_node_ != kNone ? net_.node(hover_node_).pos : world);
            } else if (drawing_ && in.down(Button::MouseLeft)) {
                if (distance(world, stroke_.back()) > 2.0f) stroke_.push_back(world);
            } else if (drawing_ && release) {
                stroke_.push_back(world);
                finish_stroke(ctx);
                drawing_ = false;
                stroke_.clear();
                stroke_from_ = kNone;
            }
        } else if (tool_ == kToolBulldoze) {
            if (release && !on_hud) {
                Vec2 at{};
                bool removed = false;
                if (hover_node_ != kNone) {
                    at = net_.node(hover_node_).pos;
                    net_.remove_node(hover_node_);
                    removed = true;
                } else if (hover_edge_ != kNone) {
                    at = closest_on_edge(hover_edge_, world);
                    net_.remove_edge(hover_edge_);
                    removed = true;
                }
                if (removed) {
                    rebuild_keeping_traffic();
                    ctx.emit(kEvRemoved, at.x, at.y);
                }
            }
        }
    }

    float snap_range() const { return std::max(px(14), 6.0f); }

    Vec2 closest_on_edge(u32 edge, Vec2 p) const {
        PathSampler s(net_.edge(edge).path, 0.5f);
        return s.closest(p).position;
    }

    /// Turn the drawn stroke into a road.
    void finish_stroke(const TickContext& ctx) {
        if (stroke_.size() < 2) return;
        float drawn = 0;
        for (usize i = 1; i < stroke_.size(); ++i) drawn += distance(stroke_[i], stroke_[i - 1]);
        const RoadType* type = road_type_.get(roads_);
        if (drawn < 12 || !type) return;

        const float radius = std::max(type->width() * 0.6f, 3.0f);
        const u32 a = stroke_from_ != kNone && net_.valid_node(stroke_from_) ? stroke_from_
                                                                            : net_.add_node(stroke_.front(), 0, 0, radius);
        const u32 end = net_.nearest_node(stroke_.back(), snap_range());
        const u32 b = end != kNone && end != a ? end : net_.add_node(stroke_.back(), 0, 0, radius);
        if (a == b || !net_.valid_node(a) || !net_.valid_node(b)) return;

        // Pin the ends so the fitted curve starts and ends on the junctions.
        stroke_.front() = net_.node(a).pos;
        stroke_.back() = net_.node(b).pos;
        Path path = fit_path(simplify_polyline(stroke_, 0.4f), 1.2f);
        if (path.empty()) return;

        const u32 edge = net_.add_edge(a, b, &path);
        if (edge == kNone) return;
        net_.set_tracks(edge, type->lanes_forward, type->lanes_backward, type->lane_width);
        grow_junction(a, radius);
        grow_junction(b, radius);
        net_.rebuild();

        PathSampler fitted(net_.edge(edge).path, 0.5f);
        const Vec2 mid = fitted.at(fitted.length() * 0.5f).position;
        ctx.emit(kEvBuilt, mid.x, mid.y);
    }

    void grow_junction(u32 node, float radius) {
        if (net_.valid_node(node) && net_.node(node).radius < radius) net_.node(node).radius = radius;
    }

    // ---- traffic ------------------------------------------------------------

    u32 pick_type() {
        float total = 0;
        for (const VehicleType& t : vehicles_.rows()) total += std::max(t.share, 0.0f);
        if (total <= 0) return 0;
        float pick = rng_.range_f(0, total);
        for (usize i = 0; i < vehicles_.size(); ++i) {
            pick -= std::max(vehicles_.at(i).share, 0.0f);
            if (pick <= 0) return static_cast<u32>(i);
        }
        return 0;
    }

    bool room_on(u32 track_index, float length) const {
        for (u32 id : cars_.on_track(track_index))
            if (cars_.agent(id).distance < length + 8.0f) return false;
        return true;
    }

    NetAgentParams params_of(const VehicleType& type) const {
        NetAgentParams p;
        p.length = type.length;
        p.max_speed = type.top_speed_kmh / 3.6f;
        p.accel = type.accel;
        p.decel = type.decel;
        p.min_gap = 1.6f;
        p.headway = 0.9f;
        p.change_length = 22;
        return p;
    }

    void spawn_car() {
        const usize tracks = net_.track_count();
        if (tracks < 2 || cars_.count() >= max_cars_ || vehicles_.size() == 0) return;
        const u32 type_index = pick_type();
        const VehicleType& type = vehicles_.at(type_index);
        for (int attempt = 0; attempt < 6; ++attempt) {
            const u32 from = rng_.below(static_cast<u32>(tracks));
            const u32 to = rng_.below(static_cast<u32>(tracks));
            if (from == to || !room_on(from, type.length)) continue;
            const Route route = net_.route(net_.track_ref(from), net_.track_ref(to));
            if (!route.found || route.steps.empty()) continue;
            const u32 id = cars_.spawn(route, 0, params_of(type), type_index);
            const TrackRef dest = net_.track_ref(to);
            if (id >= car_info_.size()) car_info_.resize(id + 1);
            car_info_[id] = CarInfo{type_index, dest.edge, dest.track};
            ++spawned_;
            return;
        }
    }

    /// The network changed shape, so every route and every track index in it
    /// is stale. Put the cars back where they physically are and re-plan.
    void rebuild_keeping_traffic() {
        struct Standing {
            Vec2 pos;
            float speed;
            CarInfo info;
        };
        std::vector<Standing> standing;
        for (u32 i = 0; i < static_cast<u32>(cars_.agents().size()); ++i) {
            if (!cars_.valid(i) || i >= car_info_.size()) continue;
            standing.push_back(Standing{cars_.pose(net_, i).position, cars_.agent(i).speed, car_info_[i]});
        }
        net_.rebuild();
        cars_.clear();
        car_info_.clear();

        for (const Standing& s : standing) {
            if (!net_.valid_edge(s.info.dest_edge)) continue;
            const NetEdge& dest_edge = net_.edge(s.info.dest_edge);
            if (s.info.dest_track >= dest_edge.tracks.size()) continue;
            Location from{};
            if (!net_.locate(s.pos, from, 8.0f)) continue;
            const TrackRef dest{s.info.dest_edge, s.info.dest_track};
            const Route route = net_.route(from, Location{dest, net_.track_length(dest)});
            if (!route.found) continue;
            if (s.info.type >= vehicles_.size()) continue;
            const u32 id = cars_.spawn(route, from.distance, params_of(vehicles_.at(s.info.type)), s.info.type);
            cars_.agent(id).speed = s.speed;
            if (id >= car_info_.size()) car_info_.resize(id + 1);
            car_info_[id] = s.info;
        }
    }

    void clear_traffic() {
        cars_.clear();
        car_info_.clear();
    }

    void build_starting_grid() {
        const RoadType* type = road_type_.get(roads_);
        const u32 lanes_f = type ? type->lanes_forward : 1;
        const u32 lanes_b = type ? type->lanes_backward : 1;
        const float lane_w = type ? type->lane_width : 3.5f;
        const float radius = std::max((static_cast<float>(lanes_f + lanes_b) * lane_w) * 0.6f, 3.0f);

        constexpr int kBlocks = 3;
        constexpr float kBlock = 140;
        u32 grid[kBlocks + 1][kBlocks + 1];
        for (int y = 0; y <= kBlocks; ++y)
            for (int x = 0; x <= kBlocks; ++x)
                grid[y][x] = net_.add_node({static_cast<float>(x) * kBlock, static_cast<float>(y) * kBlock}, 0, 0, radius);
        for (int y = 0; y <= kBlocks; ++y) {
            for (int x = 0; x <= kBlocks; ++x) {
                if (x < kBlocks) net_.set_tracks(net_.add_edge(grid[y][x], grid[y][x + 1]), lanes_f, lanes_b, lane_w);
                if (y < kBlocks) net_.set_tracks(net_.add_edge(grid[y][x], grid[y + 1][x]), lanes_f, lanes_b, lane_w);
            }
        }
        net_.rebuild();
    }

    usize live_edges() const {
        usize n = 0;
        for (const NetEdge& e : net_.edges()) n += e.alive ? 1 : 0;
        return n;
    }

    float network_length() const {
        float total = 0;
        for (const NetEdge& e : net_.edges())
            if (e.alive) total += e.path.length();
        return total;
    }

    // ---- drawing ------------------------------------------------------------

    void draw_roads(Renderer2D& r) {
        // Asphalt first, then the junction discs that cover the seams, then
        // the markings on top of both.
        for (const NetEdge& e : net_.edges()) {
            if (!e.alive) continue;
            StrokeStyle s;
            s.width = e.width;
            s.join = LineJoin::Round;
            s.cap = LineCap::Butt;
            set_color(s, kAsphalt);
            r.draw_path(e.path, s);
        }
        for (const NetNode& n : net_.nodes()) {
            if (!n.alive) continue;
            float radius = n.radius;
            for (u32 e : net_.edges_at(node_index(n))) radius = std::max(radius, net_.edge(e).width * 0.5f);
            r.fill_circle(n.pos.x, n.pos.y, radius, kAsphalt, 18);
        }
        for (const NetEdge& e : net_.edges())
            if (e.alive) draw_markings(r, e);
    }

    /// Kerbs, the line between the directions and dashed lane dividers,
    /// stopping short of the junctions at both ends.
    void draw_markings(Renderer2D& r, const NetEdge& e) {
        const float length = e.path.length();
        const float head = net_.valid_node(e.a) ? net_.node(e.a).radius : 0;
        const float tail = net_.valid_node(e.b) ? net_.node(e.b).radius : 0;
        if (length - head - tail < 2) return;
        const Path body = e.path.sub_path(head, length - tail);
        if (body.empty()) return;
        const float half = e.width * 0.5f;

        StrokeStyle kerb;
        kerb.width = std::max(0.3f, px(1));
        set_color(kerb, kKerb);
        r.draw_polyline(body.offset(half - 0.15f), kerb);
        r.draw_polyline(body.offset(-(half - 0.15f)), kerb);

        // Where the two directions meet, and the dividers inside each of them.
        float forward_inner = 1e30f, backward_inner = -1e30f;
        std::vector<float> forward_offsets, backward_offsets;
        for (const Track& t : e.tracks) {
            (t.forward ? forward_offsets : backward_offsets).push_back(t.offset);
            if (t.forward) forward_inner = std::min(forward_inner, t.offset);
            else backward_inner = std::max(backward_inner, t.offset);
        }
        if (!forward_offsets.empty() && !backward_offsets.empty()) {
            StrokeStyle centre;
            centre.width = std::max(0.28f, px(1));
            set_color(centre, kCentreLine);
            r.draw_polyline(body.offset((forward_inner + backward_inner) * 0.5f), centre);
        }
        StrokeStyle lane;
        lane.width = std::max(0.22f, px(1));
        lane.dash = 4;
        lane.gap = 5;
        set_color(lane, kLaneLine);
        for (std::vector<float>* side : {&forward_offsets, &backward_offsets}) {
            std::sort(side->begin(), side->end());
            for (usize i = 1; i < side->size(); ++i)
                r.draw_polyline(body.offset(((*side)[i - 1] + (*side)[i]) * 0.5f), lane);
        }
    }

    void draw_cars(Renderer2D& r) {
        r.set_layer(1);
        for (u32 i = 0; i < static_cast<u32>(cars_.agents().size()); ++i) {
            if (!cars_.valid(i)) continue;
            const NetAgent& a = cars_.agent(i);
            const PathPoint p = cars_.pose(net_, i);
            const VehicleType* type = a.tag < vehicles_.size() ? &vehicles_.at(a.tag) : nullptr;
            const float length = type ? type->length : a.params.length;
            const float width = type ? type->width : 1.8f;
            const Vec2 dir = p.direction;
            const Vec2 side = perp(dir) * (width * 0.5f);
            const Vec2 front = p.position;
            const Vec2 back = front - dir * length;
            const Color body = kCarColors[(type ? type->color : 0) % 8];
            r.fill_polygon({front + side, front - side, back - side, back + side}, body);
            // A lighter nose so the direction reads at a glance, and brake
            // lights while the car is stopped or giving way.
            const Vec2 nose = front - dir * std::min(1.2f, length * 0.3f);
            r.fill_polygon({front + side, front - side, nose - side, nose + side}, rgba(255, 255, 255, 60));
            if (a.speed < 1.0f) {
                const Color light = a.waiting ? rgb(255, 170, 70) : rgb(230, 70, 70);
                r.fill_circle(back.x + side.x * 0.6f, back.y + side.y * 0.6f, std::max(0.35f, px(1)), light, 6);
                r.fill_circle(back.x - side.x * 0.6f, back.y - side.y * 0.6f, std::max(0.35f, px(1)), light, 6);
            }
        }
        r.set_layer(0);
    }

    void draw_tool(Renderer2D& r) {
        r.set_layer(2);
        if (tool_ == kToolBuild) {
            const RoadType* type = road_type_.get(roads_);
            if (drawing_ && stroke_.size() >= 2) {
                StrokeStyle ghost;
                ghost.width = type ? type->width() : 7;
                ghost.join = LineJoin::Round;
                ghost.cap = LineCap::Round;
                set_color(ghost, rgba(255, 214, 90, 70));
                r.draw_polyline(stroke_, ghost);
                StrokeStyle line;
                line.width = std::max(0.4f, px(1.5f));
                set_color(line, kGhost);
                r.draw_polyline(stroke_, line);
            }
            if (hover_node_ != kNone) {
                const NetNode& n = net_.node(hover_node_);
                r.fill_circle(n.pos.x, n.pos.y, std::max(n.radius, px(6)), rgba(255, 214, 90, 90), 16);
            }
        } else if (tool_ == kToolBulldoze) {
            if (hover_node_ != kNone) {
                const NetNode& n = net_.node(hover_node_);
                r.fill_circle(n.pos.x, n.pos.y, std::max(n.radius, px(6)), kBad, 16);
            } else if (hover_edge_ != kNone) {
                const NetEdge& e = net_.edge(hover_edge_);
                StrokeStyle s;
                s.width = e.width;
                s.join = LineJoin::Round;
                set_color(s, kBad);
                r.draw_path(e.path, s);
            }
        }
        r.set_layer(0);
    }

    void draw_hud(Renderer2D& r) {
        if (!app_) return;
        ui_.begin(&r, app_->ui_input());

        ui_.begin_panel(kToolPanel, "Build");
        bool build = tool_ == kToolBuild, bulldoze = tool_ == kToolBulldoze;
        if (ui_.toggle("Road", build)) app_->push_command(kCmdTool, kToolBuild);
        ui_.same_line();
        if (ui_.toggle("Bulldoze", bulldoze)) app_->push_command(kCmdTool, kToolBulldoze);
        ui_.spacing(2);
        for (usize i = 0; i < roads_.size(); ++i) {
            const RoadType& type = roads_.at(i);
            char label[64];
            std::snprintf(label, sizeof label, "%s  %ux%u", type.id.c_str(), type.lanes_forward, type.lanes_backward);
            bool selected = type.id == road_type_.id;
            if (ui_.toggle(label, selected)) app_->push_command(kCmdRoad, static_cast<i32>(i));
        }
        ui_.end_panel();

        ui_.begin_panel(kInfoPanel, "City");
        char line[80];
        std::snprintf(line, sizeof line, "%zu cars on the road", cars_.count());
        ui_.label(line);
        std::snprintf(line, sizeof line, "%u arrived, %u sent", arrived_, spawned_);
        ui_.label(line, true);
        std::snprintf(line, sizeof line, "%zu roads, %.1f km", live_edges(), network_length() / 1000);
        ui_.label(line, true);
        ui_.progress(static_cast<float>(cars_.count()) / static_cast<float>(max_cars_ ? max_cars_ : 1), 186, {}, "traffic");
        TimeControl& time = app_->time_control();
        const float scales[3] = {1, 2, 4};
        const char* names[3] = {"1x", "2x", "4x"};
        for (int i = 0; i < 3; ++i) {
            if (i > 0) ui_.same_line();
            bool on = std::fabs(time.time_scale - scales[i]) < 1e-3f;
            if (ui_.toggle(names[i], on)) time.time_scale = scales[i];
        }
        if (ui_.button("Clear traffic", 186)) app_->push_command(kCmdClearCars);
        ui_.end_panel();

        ui_.end();

        r.draw_text(12, kHeight - 44,
                    tool_ == kToolBuild ? "drag: draw a road   right click: cancel, drag to pan   wheel: zoom"
                                        : "click a road or junction to remove it   right drag: pan   wheel: zoom",
                    rgb(150, 152, 165));
        r.draw_text(12, kHeight - 28, "hold F4: rewind   F2: pause   F5: replay + verify   F1: overlay", rgb(110, 112, 125));
    }

    static void set_color(StrokeStyle& s, Color c) {
        s.r = c.r / 255.0f;
        s.g = c.g / 255.0f;
        s.b = c.b / 255.0f;
        s.a = c.a / 255.0f;
    }

    u32 node_index(const NetNode& n) const { return static_cast<u32>(&n - net_.nodes().data()); }

    // ---- state --------------------------------------------------------------

    CurveNetwork net_;
    NetworkAgents cars_;
    std::vector<CarInfo> car_info_;
    Rng rng_{20260907};
    u32 tool_ = kToolBuild;
    DefRef<RoadType> road_type_{"street"};
    std::vector<Vec2> stroke_;
    u32 stroke_from_ = kNone;
    bool drawing_ = false;
    Vec2 cam_center_{210, 210};
    Vec2 cam_target_{210, 210};
    float zoom_ = 1.7f;
    float zoom_target_ = 1.7f;
    float spawn_accum_ = 0;
    float spawn_interval_ = 0.5f;
    u32 max_cars_ = 400;
    u32 spawned_ = 0;
    u32 arrived_ = 0;
    InputFrame prev_input_;

    // Derived per tick, not saved.
    u32 hover_node_ = kNone;
    u32 hover_edge_ = kNone;
    float start_zoom_ = 1.7f;
    float start_spawn_ = 0.5f;

    // Data, not simulation state.
    DataTable<VehicleType> vehicles_{"vehicles", "vehicles.json"};
    DataTable<RoadType> roads_{"road_types", "road_types.json"};

    // Presentation.
    App* app_ = nullptr;
    Ui ui_;
    ParticleSystem dust_{11};
    NetworkTool net_tool_{&net_, "Roads"};
    ParticleDef build_puff_ = [] {
        ParticleDef d;
        d.id = "build";
        d.lifetime_min = 0.3f;
        d.lifetime_max = 0.9f;
        d.speed_min = 2;
        d.speed_max = 10;
        d.radius = 4;
        d.drag = 2;
        d.size_start = 3;
        d.size_end = 6;
        d.color_start = rgba(200, 200, 190, 160);
        d.color_end = rgba(150, 150, 150, 0);
        return d;
    }();
    ParticleDef rubble_ = [] {
        ParticleDef d;
        d.id = "rubble";
        d.lifetime_min = 0.4f;
        d.lifetime_max = 1.0f;
        d.speed_min = 4;
        d.speed_max = 18;
        d.drag = 2.5f;
        d.size_start = 2;
        d.size_end = 0;
        d.color_start = rgba(180, 140, 110, 220);
        d.color_end = rgba(120, 100, 90, 0);
        d.burst = 22;
        return d;
    }();
};

REWIND_GAME(Traffic)
