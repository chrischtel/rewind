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

const Color kGround = rgb(96, 112, 70);
const Color kGrassDark = rgb(88, 104, 62);
const Color kGrassLight = rgb(128, 142, 86);
const Color kSoil = rgb(150, 136, 100);
const Color kTreeDark = rgb(46, 70, 42);
const Color kTreeLight = rgb(92, 122, 62);
const Color kTreeShadow = rgba(30, 42, 26, 80);
const Color kTownGround = rgb(150, 138, 118);
const Color kTownRoof = rgb(122, 92, 78);
const Color kTownLinked = rgb(120, 200, 130);
const Color kTownLoose = rgb(210, 120, 110);
/// European place-name plates: yellow for a village, blue for a main road
/// destination, green for a trunk road, white for anything local.
const Color kPlate[4] = {rgb(226, 190, 60), rgb(40, 82, 158), rgb(30, 110, 76), rgb(96, 100, 110)};
constexpr float kTownReach = 140.0f; ///< how far a town looks for a road to join
const Color kAsphalt = rgb(58, 57, 57);
/// Markings are white in Europe, never yellow, and their sizes are real
/// metres so they stay right at every zoom.
const Color kMarkingWhite = rgba(232, 232, 226, 235);
constexpr float kEdgeLineWidth = 0.25f;   ///< a motorway edge line really is 0.30 m
constexpr float kCentreLineWidth = 0.25f;
constexpr float kLaneLineWidth = 0.20f;
constexpr float kCentreDash = 4.0f; ///< Leitlinie: 4 m painted
constexpr float kCentreGap = 8.0f;  ///< and 8 m clear
constexpr float kLaneDash = 3.0f;
constexpr float kLaneGap = 6.0f;
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

/// A town, from assets/towns.json. Towns are why the traffic exists: every
/// pair of them wants to be connected, and cars are the trips between them.
struct Town {
    std::string id;
    std::string name;
    float x = 0;
    float y = 0;
    u32 size = 2;   ///< how much traffic it generates and attracts
    u32 plate = 0;  ///< colour of its name plate

    Vec2 pos() const { return {x, y}; }

    void reflect(Visitor& v) {
        v.field("id", id);
        v.field("name", name);
        v.field("x", x);
        v.field("y", y);
        v.field("size", size, {.min = 1, .max = 10});
        v.field("plate", plate, {.min = 0, .max = 3});
    }
};

/// Where a town meets the road network, found again whenever the network
/// changes. Simulation state: the trips depend on it.
struct TownAccess {
    bool has = false;
    u32 edge = 0;
    u32 track = 0;
    float distance = 0;

    void reflect(Visitor& v) {
        v.field("has", has);
        v.field("edge", edge);
        v.field("track", track);
        v.field("distance", distance);
    }
};

/// One pair of towns that wants to be connected, and how well it is served.
struct RouteDemand {
    u32 from = 0;
    u32 to = 0;
    bool connected = false;
    float pending = 0;      ///< fractional trips waiting to be sent
    u32 sent = 0;
    u32 arrived = 0;
    float last_minutes = 0; ///< travel time of the last car to arrive
    float best_minutes = 0; ///< the quickest anyone has managed

    void reflect(Visitor& v) {
        v.field("from", from);
        v.field("to", to);
        v.field("connected", connected);
        v.field("pending", pending);
        v.field("sent", sent);
        v.field("arrived", arrived);
        v.field("last_minutes", last_minutes);
        v.field("best_minutes", best_minutes);
    }
};

/// What the game knows about a car beyond what the agent itself carries.
/// Indexed like the agents, so it survives in the saved state.
struct CarInfo {
    u32 type = 0;
    u32 dest_edge = 0; ///< a TrackRef: edge indices stay valid across rebuilds
    u32 dest_track = 0;
    u32 route = kNone; ///< which town pair this trip serves
    u64 started = 0;   ///< tick it set off, for the travel time

    void reflect(Visitor& v) {
        v.field("type", type);
        v.field("dest_edge", dest_edge);
        v.field("dest_track", dest_track);
        v.field("route", route);
        v.field("started", started);
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
            if (cfg.args[i] == "--zoom") start_zoom_ = clampf(std::stof(cfg.args[i + 1]), 0.3f, 24.0f);
            if (cfg.args[i] == "--demand") start_demand_ = clampf(std::stof(cfg.args[i + 1]), 0.1f, 60.0f);
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
        if (!towns_.load(&error)) RW_LOG_WARN("traffic", "%s", error.c_str());
    }

    std::vector<DataTableBase*> data_tables() override { return {&vehicles_, &roads_, &towns_}; }
    /// The editor gets the engine's road tool for the same network.
    std::vector<EditorTool*> editor_tools() override { return {&net_tool_}; }

    void init() override {
        net_.clear();
        cars_.clear();
        car_info_.clear();
        rng_ = Rng(20260907);
        spawned_ = arrived_ = 0;
        routes_.clear();
        access_.clear();
        access_version_ = ~0ull;
        drawing_ = false;
        stroke_.clear();
        stroke_from_ = kNone;
        tool_ = kToolBuild;
        road_type_ = DefRef<RoadType>{roads_.size() ? roads_.id_at(0) : std::string("street")};
        build_starting_map();
        cam_target_ = cam_center_ = {0, 0};
        zoom_target_ = zoom_ = start_zoom_;
        clock_ = 0;
        demand_ = start_demand_;
    }

    // ---- simulation ---------------------------------------------------------

    void tick(const TickContext& ctx) override {
        handle_command(ctx.input);
        update_camera(ctx.input, ctx.dt);
        update_tool(ctx);
        prev_input_ = ctx.input;

        clock_ += ctx.dt;
        for (u32 i = 0; i < static_cast<u32>(cars_.agents().size()); ++i) {
            if (!cars_.valid(i) || !cars_.agent(i).done) continue;
            arrive(i, ctx.tick);
            cars_.remove(i);
        }
        if (access_version_ != net_.version()) refresh_connections();
        send_trips(ctx);
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
        v.field("town_access", access_);
        v.field("routes", routes_);
        v.field("access_version", access_version_);
        v.field("clock", clock_);
        v.field("demand", demand_, {.min = 0.1f, .max = 20, .speed = 0.1f});
        v.field("max_cars", max_cars_, {.min = 0, .max = 4000});
        v.field("spawned", spawned_, {.read_only = true});
        v.field("arrived", arrived_, {.read_only = true});
        v.field("prev_input", prev_input_);
    }

    // ---- rendering ----------------------------------------------------------

    void render(Renderer2D& r, float /*alpha*/) override {
        r.clear(kGround);
        r.set_camera(Camera::centered(cam_center_, zoom_, kWidth, kHeight));

        draw_ground(r);
        draw_roads(r);
        draw_cars(r);
        draw_towns(r);
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
            zoom_target_ = clampf(zoom_target_ * std::pow(1.2f, static_cast<float>(in.wheel)), 0.35f, 24.0f);
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

    /// Is there space to put a car down at `distance` along this track?
    bool room_at(TrackRef track, float distance, float length) const {
        const u32 index = net_.track_index(track);
        if (index == kNone) return false;
        for (u32 id : cars_.on_track(index)) {
            const NetAgent& a = cars_.agent(id);
            if (std::fabs(a.distance - distance) < length + 8.0f) return false;
        }
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

    // ---- towns, routes and trips ---------------------------------------------
    //
    // Traffic is not random: every car is a trip between two towns that want
    // to be connected. A route with no road between its towns sends nothing,
    // which is exactly what the player is there to fix.

    Vec2 town_pos(u32 i) const { return towns_.size() ? towns_.at(i).pos() : Vec2{}; }

    /// Find each town's way onto the network again, then re-check which town
    /// pairs can reach each other. Runs only when the network changed.
    void refresh_connections() {
        access_version_ = net_.version();
        access_.assign(towns_.size(), TownAccess{});
        for (usize i = 0; i < towns_.size(); ++i) {
            Location at{};
            if (!net_.locate(town_pos(static_cast<u32>(i)), at, kTownReach)) continue;
            access_[i] = TownAccess{true, at.track.edge, at.track.track, at.distance};
        }
        if (routes_.empty()) build_routes();
        for (RouteDemand& r : routes_) r.connected = plan(r).found;
    }

    void build_routes() {
        routes_.clear();
        for (u32 a = 0; a < static_cast<u32>(towns_.size()); ++a)
            for (u32 b = a + 1; b < static_cast<u32>(towns_.size()); ++b) routes_.push_back(RouteDemand{a, b});
    }

    /// The way from one town onto the network, out again at the other.
    Route plan(const RouteDemand& r, bool reverse = false) const {
        const u32 from = reverse ? r.to : r.from;
        const u32 to = reverse ? r.from : r.to;
        if (from >= access_.size() || to >= access_.size()) return Route{};
        const TownAccess& a = access_[from];
        const TownAccess& b = access_[to];
        if (!a.has || !b.has || !net_.valid_edge(a.edge) || !net_.valid_edge(b.edge)) return Route{};
        return net_.route(Location{TrackRef{a.edge, a.track}, a.distance}, Location{TrackRef{b.edge, b.track}, b.distance});
    }

    void send_trips(const TickContext& ctx) {
        if (vehicles_.size() == 0 || towns_.size() == 0) return;
        for (usize i = 0; i < routes_.size(); ++i) {
            RouteDemand& r = routes_[i];
            if (!r.connected) continue;
            // Both towns pull: a big town sends and receives more.
            const float weight = static_cast<float>(towns_.at(r.from).size + towns_.at(r.to).size) * 0.5f;
            r.pending += weight * demand_ / 60.0f * ctx.dt;
            r.pending = std::min(r.pending, 4.0f); // a jam must not build up an infinite backlog
            while (r.pending >= 1.0f) {
                if (cars_.count() >= max_cars_) return;
                if (!send_one(static_cast<u32>(i), ctx.tick)) break; // no room at the kerb, try again next tick
                r.pending -= 1.0f;
            }
        }
    }

    /// One trip, in whichever direction comes up. Returns false when the car
    /// could not be placed, so the trip stays pending instead of vanishing.
    bool send_one(u32 route_index, u64 tick) {
        RouteDemand& r = routes_[route_index];
        const bool reverse = (rng_.next_u32() & 1u) != 0;
        const Route route = plan(r, reverse);
        if (!route.found || route.steps.empty()) return false;
        const u32 type_index = pick_type();
        const VehicleType& type = vehicles_.at(type_index);
        const TrackRef first = net_.track_ref(route.steps.front().index);
        const float start = reverse ? access_[r.to].distance : access_[r.from].distance;
        if (!room_at(first, start, type.length)) return false;
        const u32 id = cars_.spawn(route, start, params_of(type), type_index);
        const TownAccess& dest = access_[reverse ? r.from : r.to];
        if (id >= car_info_.size()) car_info_.resize(id + 1);
        car_info_[id] = CarInfo{type_index, dest.edge, dest.track, route_index, tick};
        ++r.sent;
        ++spawned_;
        return true;
    }

    void arrive(u32 car, u64 tick) {
        ++arrived_;
        if (car >= car_info_.size()) return;
        const CarInfo& info = car_info_[car];
        if (info.route >= routes_.size()) return;
        RouteDemand& r = routes_[info.route];
        ++r.arrived;
        const float minutes = static_cast<float>(tick - info.started) / 60.0f / 60.0f;
        r.last_minutes = minutes;
        r.best_minutes = r.best_minutes <= 0 ? minutes : std::min(r.best_minutes, minutes);
    }

    usize routes_connected() const {
        usize n = 0;
        for (const RouteDemand& r : routes_) n += r.connected ? 1 : 0;
        return n;
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

    /// Every town starts with a short piece of road pointing at the middle of
    /// the map. Connecting them is the game.
    void build_starting_map() {
        const RoadType* type = road_type_.get(roads_);
        const u32 lanes_f = type ? type->lanes_forward : 1;
        const u32 lanes_b = type ? type->lanes_backward : 1;
        const float lane_w = type ? type->lane_width : 3.5f;
        const float radius = std::max((static_cast<float>(lanes_f + lanes_b) * lane_w) * 0.6f, 3.0f);
        for (usize i = 0; i < towns_.size(); ++i) {
            const Vec2 at = town_pos(static_cast<u32>(i));
            Vec2 inward = normalize(Vec2{0, 0} - at);
            if (length_sq(inward) < 0.5f) inward = {1, 0};
            const u32 a = net_.add_node(at + inward * 26.0f, 0, 0, radius);
            const u32 b = net_.add_node(at + inward * 90.0f, 0, 0, radius);
            net_.set_tracks(net_.add_edge(a, b), lanes_f, lanes_b, lane_w);
        }
        net_.rebuild();
        refresh_connections();
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

    /// Grass, generated in a shader from world metres, plus scattered trees.
    /// All presentation: nothing here is ever read by the simulation.
    void draw_ground(Renderer2D& r) {
        const Rect view = visible_world();
        if (!ground_material_) ground_material_ = r.load_material("shaders/ground.frag.spv");

        r.set_layer(-20);
        if (ground_material_ && ground_material_->ok) {
            struct Params {
                float dark[4];
                float light[4];
                float soil[4];
                float detail[2];
                float pad[2];
            } params{};
            fill_rgb(params.dark, kGrassDark);
            fill_rgb(params.light, kGrassLight);
            fill_rgb(params.soil, kSoil);
            params.detail[0] = 26.0f;                          // metres per noise cell
            params.detail[1] = clampf(zoom_ / 6.0f, 0.0f, 1.0f); // fine speckle fades out when zoomed away
            r.set_material(ground_material_);
            r.set_material_params(params);
            // One quad over the view, its uv carrying world metres.
            Mesh quad;
            const u32 a = quad.add_vertex({view.x, view.y, 0}, {view.x, view.y}, 1, 1, 1, 1);
            const u32 b = quad.add_vertex({view.right(), view.y, 0}, {view.right(), view.y}, 1, 1, 1, 1);
            const u32 c = quad.add_vertex({view.right(), view.bottom(), 0}, {view.right(), view.bottom()}, 1, 1, 1, 1);
            const u32 d = quad.add_vertex({view.x, view.bottom(), 0}, {view.x, view.bottom()}, 1, 1, 1, 1);
            quad.add_quad(a, b, c, d);
            r.draw_mesh(quad);
            r.set_material(nullptr);
        } else {
            r.fill_rect(view, kGrassDark);
        }

        draw_trees(r, view);
        r.set_layer(0);
    }

    /// Trees are scattered by hashing their grid cell, so the same cell always
    /// grows the same tree: no state, no memory, stable across a rewind.
    void draw_trees(Renderer2D& r, const Rect& view) {
        if (zoom_ < 0.9f) return; // too far out to be anything but noise
        constexpr float kCell = 34.0f;
        const int x0 = static_cast<int>(std::floor(view.x / kCell)) - 1;
        const int x1 = static_cast<int>(std::floor(view.right() / kCell)) + 1;
        const int y0 = static_cast<int>(std::floor(view.y / kCell)) - 1;
        const int y1 = static_cast<int>(std::floor(view.bottom() / kCell)) + 1;
        if ((x1 - x0) * (y1 - y0) > 4000) return; // sanity bound while zooming out

        r.set_layer(-19);
        for (int cy = y0; cy <= y1; ++cy) {
            for (int cx = x0; cx <= x1; ++cx) {
                const u32 h = cell_hash(cx, cy);
                if ((h & 3u) != 3u) continue; // only a quarter of the cells grow anything
                const float fx = static_cast<float>(cx) * kCell + unit(h) * kCell;
                const float fy = static_cast<float>(cy) * kCell + unit(h >> 8) * kCell;
                const Vec2 at{fx, fy};
                if (near_road(at, 9.0f)) continue; // trees do not grow on the carriageway
                const float spread = unit(h >> 16);
                const float radius = 2.4f + spread * spread * 5.0f; // many small, a few large
                const float tint = unit(h >> 20);
                r.fill_circle(at.x + 2.0f, at.y + 2.6f, radius, kTreeShadow, 10);
                const Color canopy = lerp_color(kTreeDark, kTreeLight, tint);
                r.fill_circle(at.x, at.y, radius, canopy, 10);
                r.fill_circle(at.x - radius * 0.32f, at.y - radius * 0.32f, radius * 0.55f,
                              lerp_color(canopy, kTreeLight, 0.5f), 8);
            }
        }
    }

    bool near_road(Vec2 p, float distance) const {
        const u32 e = net_.nearest_edge(p, 1e30f);
        if (e == kNone) return false;
        const NetEdge& edge = net_.edge(e);
        return distance_to_edge(edge, p) < edge.width * 0.5f + distance;
    }

    static float distance_to_edge(const NetEdge& e, Vec2 p) {
        // Cheap: the flattened centreline is enough for scattering decisions.
        std::vector<Vec2> line;
        e.path.flatten(1.5f, line);
        float best = 1e30f;
        for (usize i = 1; i < line.size(); ++i) best = std::min(best, distance(closest_point_on_segment(line[i - 1], line[i], p), p));
        return best;
    }

    Rect visible_world() const {
        const Camera c = view(cam_center_, zoom_);
        const Vec2 top_left = c.to_world({0, 0});
        const Vec2 bottom_right = c.to_world({kWidth, kHeight});
        return Rect{top_left.x, top_left.y, bottom_right.x - top_left.x, bottom_right.y - top_left.y};
    }

    static u32 cell_hash(int x, int y) {
        u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(y) * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return h ^ (h >> 16);
    }
    static float unit(u32 h) { return static_cast<float>(h & 0xffffu) / 65535.0f; }
    static void fill_rgb(float (&out)[4], Color c) {
        out[0] = c.r / 255.0f;
        out[1] = c.g / 255.0f;
        out[2] = c.b / 255.0f;
        out[3] = 1;
    }
    static Color lerp_color(Color a, Color b, float t) {
        const auto mix = [t](u8 x, u8 y) { return static_cast<u8>(x + (static_cast<float>(y) - static_cast<float>(x)) * t); };
        return Color{mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), 255};
    }

    // ---- road geometry ------------------------------------------------------
    //
    // Roads are built into two meshes once and redrawn every frame. Every
    // immediate polyline call would otherwise rebuild the same geometry sixty
    // times a second. The cache is keyed on the network's version, so any
    // edit, and any rewind that changes the network, rebuilds it.

    void draw_roads(Renderer2D& r) {
        // Markings are baked in world units, so the mesh is rebuilt when the
        // zoom changes enough that the minimum on-screen width would move.
        const int bucket = static_cast<int>(std::floor(std::log2(std::max(zoom_, 0.1f)) * 2.0f));
        if (road_version_ != net_.version() || road_zoom_bucket_ != bucket) rebuild_road_mesh(bucket);
        r.set_layer(-2);
        r.draw_mesh(surface_mesh_);
        if (zoom_ >= 1.2f) r.draw_mesh(marking_mesh_);
        r.set_layer(0);
    }

    /// World units per marking, never thinner than about a pixel and a half.
    float marking_width(float metres) const { return std::max(metres, 1.6f / std::max(zoom_, 0.1f)); }

    void rebuild_road_mesh(int bucket) {
        road_version_ = net_.version();
        road_zoom_bucket_ = bucket;
        surface_mesh_.clear();
        marking_mesh_.clear();
        for (const NetEdge& e : net_.edges()) {
            if (!e.alive) continue;
            build_edge_surface(e);
            build_edge_markings(e);
        }
        for (usize i = 0; i < net_.nodes().size(); ++i)
            if (net_.node(static_cast<u32>(i)).alive) build_junction(static_cast<u32>(i));
    }

    void build_edge_surface(const NetEdge& e) {
        std::vector<Vec2> line;
        e.path.flatten(0.35f, line);
        if (line.size() < 2) return;
        StrokeStyle s;
        s.width = e.width;
        s.join = LineJoin::Miter;
        s.cap = LineCap::Butt;
        set_color(s, kAsphalt);
        build_polyline(line, s, surface_mesh_);
    }

    /// The paved area where roads meet: each arm is cut off a little way from
    /// the node and the gaps between them are closed at the point where their
    /// kerbs would cross. Much closer to a real junction than a disc.
    void build_junction(u32 node) {
        struct Arm {
            float angle;
            Vec2 at;   ///< on the centreline, one junction radius out
            Vec2 dir;  ///< away from the node
            Vec2 side; ///< perpendicular, half a carriageway wide
        };
        const NetNode& n = net_.node(node);
        std::vector<Arm> arms;
        float radius = n.radius;
        for (u32 index : net_.edges_at(node)) {
            const NetEdge& e = net_.edge(index);
            if (e.path.empty()) continue;
            Vec2 dir;
            if (e.a == node) dir = e.path.segments.front().direction(0);
            else if (e.b == node) dir = e.path.segments.back().direction(1) * -1.0f;
            else continue;
            if (length_sq(dir) < 1e-6f) continue;
            radius = std::max(radius, e.width * 0.55f);
            arms.push_back(Arm{angle_of(dir), Vec2{}, dir, perp(dir) * (e.width * 0.5f)});
        }
        if (arms.size() < 2) return; // a dead end is just the road's own end cap
        for (Arm& a : arms) a.at = n.pos + a.dir * radius;
        std::sort(arms.begin(), arms.end(), [](const Arm& a, const Arm& b) { return a.angle < b.angle; });

        std::vector<Vec2> pad;
        pad.reserve(arms.size() * 3);
        for (usize i = 0; i < arms.size(); ++i) {
            const Arm& a = arms[i];
            const Arm& next = arms[(i + 1) % arms.size()];
            pad.push_back(a.at - a.side);
            pad.push_back(a.at + a.side);
            // Where this arm's kerb and the next arm's kerb would meet.
            Vec2 corner;
            if (line_intersection(a.at + a.side, a.dir, next.at - next.side, next.dir, corner)) {
                const float reach = radius + std::max(length(a.side), length(next.side)) * 3.0f;
                if (distance(corner, n.pos) < reach) pad.push_back(corner);
            }
        }
        build_polygon(pad, kAsphalt.r / 255.0f, kAsphalt.g / 255.0f, kAsphalt.b / 255.0f, 1.0f, surface_mesh_);
    }

    /// European markings: white, sized in real metres. Edge lines along the
    /// kerbs, a centre line between the directions (dashed where overtaking is
    /// allowed, doubled on wider roads), and dashed dividers between lanes
    /// going the same way. They stop short of the junctions at both ends.
    void build_edge_markings(const NetEdge& e) {
        const float total = e.path.length();
        const float head = net_.valid_node(e.a) ? std::max(net_.node(e.a).radius, e.width * 0.55f) : 0.0f;
        const float tail = net_.valid_node(e.b) ? std::max(net_.node(e.b).radius, e.width * 0.55f) : 0.0f;
        if (total - head - tail < 3) return;
        const Path body = e.path.sub_path(head, total - tail);
        if (body.empty()) return;
        const float half = e.width * 0.5f;

        StrokeStyle edge_line;
        edge_line.width = marking_width(kEdgeLineWidth);
        set_color(edge_line, kMarkingWhite);
        add_line(body.offset(half - kEdgeLineWidth), edge_line);
        add_line(body.offset(-(half - kEdgeLineWidth)), edge_line);

        std::vector<float> forward, backward;
        for (const Track& t : e.tracks) (t.forward ? forward : backward).push_back(t.offset);
        std::sort(forward.begin(), forward.end());
        std::sort(backward.begin(), backward.end());

        if (!forward.empty() && !backward.empty()) {
            StrokeStyle centre;
            centre.width = marking_width(kCentreLineWidth);
            set_color(centre, kMarkingWhite);
            const bool wide = forward.size() > 1 || backward.size() > 1;
            const float at = (forward.front() + backward.back()) * 0.5f;
            if (wide) {
                // Two solid lines with a gap read as a hard divider from above.
                add_line(body.offset(at - kCentreLineWidth * 1.6f), centre);
                add_line(body.offset(at + kCentreLineWidth * 1.6f), centre);
            } else {
                centre.dash = kCentreDash;
                centre.gap = kCentreGap;
                add_line(body.offset(at), centre);
            }
        }

        StrokeStyle lane;
        lane.width = marking_width(kLaneLineWidth);
        lane.dash = kLaneDash;
        lane.gap = kLaneGap;
        set_color(lane, kMarkingWhite);
        for (const std::vector<float>* side : {&forward, &backward})
            for (usize i = 1; i < side->size(); ++i) add_line(body.offset(((*side)[i - 1] + (*side)[i]) * 0.5f), lane);
    }

    void add_line(const std::vector<Vec2>& points, const StrokeStyle& style) {
        if (points.size() >= 2) build_polyline(points, style, marking_mesh_);
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

    /// A town: a patch of built-up ground and a name plate, in the European
    /// style, sized in pixels so it stays readable at every zoom.
    void draw_towns(Renderer2D& r) {
        r.set_layer(3);
        for (usize i = 0; i < towns_.size(); ++i) {
            const Town& t = towns_.at(i);
            const Vec2 at = t.pos();
            const float radius = 20.0f + static_cast<float>(t.size) * 8.0f;
            r.fill_circle(at.x, at.y, radius, kTownGround, 22);
            r.fill_circle(at.x, at.y, radius * 0.62f, kTownRoof, 18);
            const bool linked = i < access_.size() && access_[i].has;
            r.fill_circle(at.x, at.y, radius * 0.2f, linked ? kTownLinked : kTownLoose, 12);
        }
        r.set_layer(4);
        for (usize i = 0; i < towns_.size(); ++i) {
            const Town& t = towns_.at(i);
            const Vec2 screen = view(cam_center_, zoom_).to_screen(t.pos());
            const Vec2 size = ui_.measure(t.name);
            const Rect plate{screen.x - size.x * 0.5f - 8, screen.y - 46, size.x + 16, size.y + 8};
            if (plate.right() < 0 || plate.x > kWidth || plate.bottom() < 0 || plate.y > kHeight) continue;
            const Camera saved = r.camera();
            r.set_camera(Camera{});
            r.fill_rect({plate.x + 2, plate.y + 3, plate.w, plate.h}, rgba(0, 0, 0, 70));
            r.fill_rect(plate, kPlate[t.plate % 4]);
            r.draw_rect(plate, rgba(255, 255, 255, 200));
            r.fill_rect({screen.x - 1, plate.bottom(), 2, 12}, rgba(255, 255, 255, 160));
            r.draw_text(plate.x + 8, plate.y + 4, t.name.c_str(), rgb(255, 255, 255), 2);
            r.set_camera(saved);
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

        ui_.begin_panel(kInfoPanel, "Network");
        char line[80];
        std::snprintf(line, sizeof line, "%zu of %zu routes connected", routes_connected(), routes_.size());
        ui_.label(line);
        std::snprintf(line, sizeof line, "%zu cars, %u arrived", cars_.count(), arrived_);
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
    Vec2 cam_center_{0, 0};
    Vec2 cam_target_{0, 0};
    float zoom_ = 0.75f;
    float zoom_target_ = 0.75f;
    std::vector<TownAccess> access_;
    std::vector<RouteDemand> routes_;
    u64 access_version_ = ~0ull;
    float clock_ = 0;    ///< seconds since the map started
    float demand_ = 3.0f; ///< trips per minute for a pair of size-1 towns
    u32 max_cars_ = 400;
    u32 spawned_ = 0;
    u32 arrived_ = 0;
    InputFrame prev_input_;

    // Derived per tick, not saved.
    u32 hover_node_ = kNone;
    u32 hover_edge_ = kNone;
    float start_zoom_ = 0.75f;
    float start_demand_ = 3.0f;

    // Data, not simulation state.
    DataTable<VehicleType> vehicles_{"vehicles", "vehicles.json"};
    DataTable<RoadType> roads_{"road_types", "road_types.json"};
    DataTable<Town> towns_{"towns", "towns.json"};

    // Presentation.
    App* app_ = nullptr;
    Material* ground_material_ = nullptr;
    Mesh surface_mesh_; ///< asphalt and junction pads, rebuilt when the network changes
    Mesh marking_mesh_;
    u64 road_version_ = ~0ull;
    int road_zoom_bucket_ = -9999;
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
