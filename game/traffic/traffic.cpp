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

#include "roads.h"

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
const Color kTownGround = rgb(132, 138, 104);
const Color kRoofDark = rgb(96, 74, 66);
const Color kRoofLight = rgb(146, 104, 84);
const Color kBuildingShadow = rgba(40, 40, 34, 70);
/// European place-name plates: yellow for a village, blue for a main road
/// destination, green for a trunk road, white for anything local.
const Color kPlate[4] = {rgb(226, 190, 60), rgb(40, 82, 158), rgb(30, 110, 76), rgb(96, 100, 110)};
constexpr float kTownReach = 140.0f; ///< how far a town looks for a road to join
constexpr float kStartingMoney = 20000;
constexpr float kMoneyTarget = 60000;
constexpr float kFarePerTrip = 12;
const Color kAsphalt = rgb(58, 57, 57);
/// Markings are white in Europe, never yellow, and their sizes are real
/// metres so they stay right at every zoom.
const Color kMarkingWhite = rgba(232, 232, 226, 235);
constexpr float kNarrowLine = 0.12f;      ///< Schmalstrich
constexpr float kEdgeLineInset = 0.20f;   ///< edge lines sit this far inside the carriageway
constexpr float kStopLineWidth = 0.50f;   ///< Haltlinie
constexpr float kStopLineSetback = 0.80f; ///< back from the junction
constexpr float kToothBase = 0.50f;       ///< give-way teeth
constexpr float kToothHeight = 0.50f;
constexpr float kToothGap = 0.50f;
constexpr float kZebraBar = 0.50f;
constexpr float kZebraGap = 0.50f;
constexpr float kZebraDepth = 4.00f;
constexpr float kZebraInset = 0.30f;
constexpr float kZebraSetback = 1.50f;
constexpr float kHatchBar = 0.30f;        ///< Sperrflaeche diagonals
constexpr float kHatchStep = 1.30f;
constexpr float kHatchLength = 18.0f;
constexpr float kKerbBand = 0.30f;
constexpr float kKerbShadowBand = 0.15f;
const Color kKerbTop = rgb(172, 170, 163);
const Color kKerbShadow = rgba(20, 22, 20, 90);
const Color kGhost = rgba(255, 214, 90, 200);
const Color kBad = rgba(255, 110, 110, 220);

/// Sixteen slots, filled the way European registrations actually run: a
/// quarter grey, a fifth white, a fifth black, then the rest.
const Color kCarColors[16] = {
    rgb(232, 233, 230), rgb(232, 233, 230), rgb(146, 150, 156), rgb(92, 96, 102),
    rgb(168, 171, 175), rgb(34, 35, 38),    rgb(34, 35, 38),    rgb(186, 190, 196),
    rgb(42, 58, 102),   rgb(58, 96, 158),   rgb(158, 38, 42),   rgb(40, 72, 56),
    rgb(176, 164, 142), rgb(226, 182, 42),  rgb(208, 104, 38),  rgb(146, 150, 156)};
const Color kGlassTint = rgb(26, 30, 38);
const Color kWindscreen = rgb(120, 146, 168);
const Color kTailLampOn = rgb(255, 64, 52);
const Color kTailLampOff = rgb(126, 28, 30);
const Color kIndicatorOn = rgb(255, 176, 40);
const Color kVehicleShadow = rgba(22, 26, 32, 95);
/// Green through amber to red, for how well a road is flowing.
const Color kFlowFree = rgb(96, 200, 104);
const Color kFlowSlow = rgb(240, 176, 64);
const Color kFlowJam = rgb(212, 62, 54);

/// Commands from the HUD. They ride in the input frame, so a click on a
/// button is recorded, rewinds and replays like a key press.
enum Command : u32 { kCmdTool = 1, kCmdRoad = 2, kCmdClearCars = 3, kCmdFlow = 4 };
enum Event : u32 { kEvBuilt = 1, kEvRemoved = 2 };

constexpr u32 kToolBuild = 0;
constexpr u32 kToolBulldoze = 1;
constexpr u32 kToolSignals = 2;
const char* const kToolNames[] = {"build", "bulldoze", "signals"};
const Color kLightRed = rgb(226, 62, 52);
const Color kLightAmber = rgb(248, 178, 44);
const Color kLightGreen = rgb(86, 206, 96);
const Color kSignalPost = rgb(48, 50, 54);

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
    float cost_per_m = 2;  ///< what a metre of it costs to build

    float width() const { return static_cast<float>(lanes_forward + lanes_backward) * lane_width; }

    void reflect(Visitor& v) {
        v.field("id", id);
        v.field("lanes_forward", lanes_forward, {.min = 0, .max = 5});
        v.field("lanes_backward", lanes_backward, {.min = 0, .max = 5});
        v.field("lane_width", lane_width, {.min = 1.5f, .max = 8});
        v.field("speed_kmh", speed_kmh, {.min = 10, .max = 160});
        v.field("cost_per_m", cost_per_m, {.min = 0, .max = 1000});
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

/// A signalled junction. Approaches are grouped into phases; a phase runs
/// green, then amber, then everything is red for a moment while the junction
/// clears. All of it is simulation state, so the lights rewind with the cars.
struct Signal {
    u32 node = 0;
    std::vector<u32> approach;       ///< incident edges, in the order they were found
    std::vector<u8> phase_of;        ///< which phase each approach belongs to
    u8 phases = 2;
    u8 phase = 0;
    float timer = 0;
    float green = 14;
    float amber = 3;
    float all_red = 1.5f;

    /// 0 red, 1 amber, 2 green.
    u8 state_of(usize approach_index) const {
        if (approach_index >= phase_of.size()) return 0;
        if (phase_of[approach_index] != phase) return 0;
        if (timer < green) return 2;
        if (timer < green + amber) return 1;
        return 0;
    }
    float cycle() const { return (green + amber + all_red) * static_cast<float>(std::max<u8>(phases, 1)); }

    void reflect(Visitor& v) {
        v.field("node", node);
        v.field("approach", approach);
        v.field("phase_of", phase_of);
        v.field("phases", phases, {.min = 1, .max = 4});
        v.field("phase", phase);
        v.field("timer", timer);
        v.field("green", green, {.min = 3, .max = 90});
        v.field("amber", amber, {.min = 1, .max = 6});
        v.field("all_red", all_red, {.min = 0, .max = 5});
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
            if (cfg.args[i] == "--demo") demo_ = std::stoi(cfg.args[i + 1]) != 0;
            if (cfg.args[i] == "--overlay-flow") start_flow_ = std::stoi(cfg.args[i + 1]) != 0;
            if (cfg.args[i] == "--at" && i + 2 < cfg.args.size()) {
                start_at_ = Vec2{std::stof(cfg.args[i + 1]), std::stof(cfg.args[i + 2])};
                has_start_at_ = true;
            }
            if (cfg.args[i] == "--demand") start_demand_ = clampf(std::stof(cfg.args[i + 1]), 0.1f, 60.0f);
        }
    }

    void start(App& app) override {
        app_ = &app;
        cars_.set_entry_rule([this](const NetworkAgents& agents, u32 agent, u32 connector) {
            return may_enter(agents, agent, connector);
        });
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
        cam_target_ = cam_center_ = has_start_at_ ? start_at_ : Vec2{0, 0};
        zoom_target_ = zoom_ = start_zoom_;
        clock_ = 0;
        money_ = kStartingMoney;
        show_flow_ = start_flow_;
        spent_ = 0;
        flow_.clear();
        signals_.clear();
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
        if (access_version_ != net_.version()) {
            sync_signals();
            refresh_connections();
        }
        step_signals(ctx.dt);
        measure_flow(ctx.dt);
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
        v.field("tool", tool_, {.names = kToolNames, .name_count = 3});
        v.field("road_type", road_type_, {.table = "road_types"});
        v.field("stroke", stroke_);
        v.field("stroke_from", stroke_from_);
        v.field("drawing", drawing_);
        v.field("cam_center", cam_center_);
        v.field("cam_target", cam_target_);
        v.field("zoom", zoom_, {.min = 1, .max = 40});
        v.field("zoom_target", zoom_target_, {.min = 1, .max = 40});
        v.field("signals", signals_);
        v.field("town_access", access_);
        v.field("routes", routes_);
        v.field("access_version", access_version_);
        v.field("clock", clock_);
        v.field("flow", flow_);
        v.field("money", money_);
        v.field("spent", spent_, {.read_only = true});
        v.field("show_flow", show_flow_);
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
        draw_flow_overlay(r);
        draw_cars(r);
        draw_signals(r);
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
        case kCmdTool: tool_ = static_cast<u32>(in.command_value) <= kToolSignals ? static_cast<u32>(in.command_value) : tool_; break;
        case kCmdRoad:
            if (in.command_value >= 0 && static_cast<usize>(in.command_value) < roads_.size())
                road_type_ = DefRef<RoadType>{roads_.id_at(static_cast<usize>(in.command_value))};
            break;
        case kCmdClearCars: clear_traffic(); break;
        case kCmdFlow: show_flow_ = !show_flow_; break;
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
        } else if (tool_ == kToolSignals) {
            if (release && !on_hud && hover_node_ != kNone) toggle_signal(hover_node_);
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

    /// Turn the drawn stroke into a road. Everything it crosses is split, so
    /// a line drawn over another road becomes a junction rather than a bridge.
    void finish_stroke(const TickContext& ctx) {
        if (stroke_.size() < 2) return;
        const RoadType* type = road_type_.get(roads_);
        if (!type) return;
        const Path path = fit_path(simplify_polyline(stroke_, 0.4f), 1.2f);
        if (path.empty()) return;

        const float price = path.length() * type->cost_per_m;
        if (price > money_) {
            refusal_ = "not enough money";
            return;
        }
        const traffic::RoadSpec spec{type->lanes_forward, type->lanes_backward, type->lane_width};
        auto standing = capture_cars(); // build_road rebuilds, which invalidates every route
        const traffic::BuildResult built = traffic::build_road(net_, path, spec, snap_range());
        if (!built.ok) {
            refusal_ = built.refusal;
            restore_cars(standing);
            return;
        }
        refusal_ = "";
        money_ -= built.length * type->cost_per_m;
        spent_ += built.length * type->cost_per_m;
        restore_cars(standing);
        refresh_connections();
        const Vec2 mid = path.sub_path(0, path.length() * 0.5f).end();
        ctx.emit(kEvBuilt, mid.x, mid.y);
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

    // ---- traffic lights ------------------------------------------------------
    //
    // A signal is a controller on a junction: the approaches are split into
    // phases and each phase gets its turn. The cars themselves know nothing
    // about it; they ask an entry rule whether they may enter a connector,
    // and the rule answers with the light.

    Signal* signal_at(u32 node) {
        for (Signal& sig : signals_)
            if (sig.node == node) return &sig;
        return nullptr;
    }
    const Signal* signal_at(u32 node) const {
        for (const Signal& sig : signals_)
            if (sig.node == node) return &sig;
        return nullptr;
    }

    /// Opposite approaches share a phase: the two arms most nearly in line
    /// with each other go green together, which is what a four-arm junction
    /// gets in real life.
    void plan_phases(Signal& sig) {
        sig.approach = net_.edges_at(sig.node);
        sig.phase_of.assign(sig.approach.size(), 0);
        std::vector<float> angle(sig.approach.size(), 0);
        for (usize i = 0; i < sig.approach.size(); ++i) {
            const NetEdge& e = net_.edge(sig.approach[i]);
            const Vec2 dir = e.a == sig.node ? e.path.segments.front().direction(0)
                                             : e.path.segments.back().direction(1) * -1.0f;
            angle[i] = angle_of(dir);
        }
        std::vector<bool> taken(sig.approach.size(), false);
        u8 next_phase = 0;
        for (usize i = 0; i < sig.approach.size(); ++i) {
            if (taken[i]) continue;
            taken[i] = true;
            sig.phase_of[i] = next_phase;
            // The arm closest to the opposite direction joins this phase.
            usize best = ~usize(0);
            float best_error = 0.6f; // radians away from straight across
            for (usize j = i + 1; j < sig.approach.size(); ++j) {
                if (taken[j]) continue;
                float difference = std::fabs(std::fabs(angle[j] - angle[i]) - 3.14159265f);
                if (difference < best_error) {
                    best_error = difference;
                    best = j;
                }
            }
            if (best != ~usize(0)) {
                taken[best] = true;
                sig.phase_of[best] = next_phase;
            }
            ++next_phase;
        }
        sig.phases = std::max<u8>(1, next_phase);
        sig.phase = 0;
        sig.timer = 0;
    }

    void toggle_signal(u32 node) {
        if (!net_.valid_node(node) || net_.edges_at(node).size() < 3) return;
        for (usize i = 0; i < signals_.size(); ++i) {
            if (signals_[i].node != node) continue;
            signals_.erase(signals_.begin() + static_cast<std::ptrdiff_t>(i));
            ++signal_version_;
            return;
        }
        Signal sig;
        sig.node = node;
        plan_phases(sig);
        signals_.push_back(sig);
        std::sort(signals_.begin(), signals_.end(), [](const Signal& a, const Signal& b) { return a.node < b.node; });
        ++signal_version_;
    }

    /// Drop signals whose junction is gone and re-plan the ones whose arms changed.
    void sync_signals() {
        const usize before = signals_.size();
        signals_.erase(std::remove_if(signals_.begin(), signals_.end(),
                                      [this](const Signal& sig) {
                                          return !net_.valid_node(sig.node) || net_.edges_at(sig.node).size() < 3;
                                      }),
                       signals_.end());
        if (signals_.size() != before) ++signal_version_;
        for (Signal& sig : signals_) {
            const std::vector<u32> arms = net_.edges_at(sig.node);
            if (arms == sig.approach) continue;
            const float keep_timer = sig.timer;
            const u8 keep_phase = sig.phase;
            plan_phases(sig);
            sig.timer = keep_timer;
            sig.phase = keep_phase < sig.phases ? keep_phase : 0;
            ++signal_version_;
        }
    }

    void step_signals(float dt) {
        for (Signal& sig : signals_) {
            sig.timer += dt;
            const float stage = sig.green + sig.amber + sig.all_red;
            if (sig.timer >= stage) {
                sig.timer -= stage;
                sig.phase = static_cast<u8>((sig.phase + 1) % std::max<u8>(sig.phases, 1));
            }
        }
    }

    /// The engine asks this before letting a car into a junction. It replaces
    /// the engine's own rule, so it has to do that job too: never enter a
    /// crossing another car is already in.
    bool may_enter(const NetworkAgents& agents, u32 agent, u32 connector) const {
        if (connector >= net_.connectors().size()) return true;
        const Connector& c = net_.connectors()[connector];

        // Whatever the light says, do not block the junction: only go if
        // there is room on the other side.
        const TrackRef out = c.to;
        const u32 out_index = net_.track_index(out);
        if (out_index != kNone) {
            const float length = agents.agent(agent).params.length + 2.0f;
            bool room = true;
            for (u32 other : agents.on_track(out_index)) {
                const NetAgent& a = agents.agent(other);
                if (a.distance < length && a.speed < 1.0f) room = false;
            }
            if (!room) return false;
        }

        for (const Conflict& conflict : net_.conflicts()) {
            if (conflict.connector_a != connector && conflict.connector_b != connector) continue;
            const u32 other = conflict.connector_a == connector ? conflict.connector_b : conflict.connector_a;
            if (!agents.on_connector(other).empty()) return false;
        }

        const Signal* sig = signal_at(c.node);
        if (!sig) return true;
        for (usize i = 0; i < sig->approach.size(); ++i) {
            if (sig->approach[i] != c.from.edge) continue;
            const u8 state = sig->state_of(i);
            if (state == 2) return true;
            // Amber: a car already close enough that stopping would be harsh
            // clears the junction instead of standing on it.
            if (state == 1) return agents.agent(agent).speed > 3.0f;
            return false;
        }
        return true;
    }

    /// How freely each lane is running, as a share of what the cars on it
    /// could do. Simulation state, so the overlay rewinds with everything else.
    void measure_flow(float dt) {
        const usize tracks = net_.track_count();
        if (flow_.size() != tracks) flow_.assign(tracks, 1.0f);
        if (tracks == 0) return;
        std::vector<float> sum(tracks, 0.0f);
        std::vector<u32> seen(tracks, 0);
        for (u32 i = 0; i < static_cast<u32>(cars_.agents().size()); ++i) {
            if (!cars_.valid(i)) continue;
            const NetAgent& a = cars_.agent(i);
            const RouteStep* step = cars_.current_step(i);
            if (!step || step->kind != RouteStep::Kind::Track || step->index >= tracks) continue;
            sum[step->index] += clampf(a.speed / std::max(a.params.max_speed, 1.0f), 0, 1);
            ++seen[step->index];
        }
        const float k = clampf(dt * 0.6f, 0, 1);
        for (usize i = 0; i < tracks; ++i) {
            const float target = seen[i] ? sum[i] / static_cast<float>(seen[i]) : 1.0f;
            flow_[i] += (target - flow_[i]) * k;
        }
    }

    /// The worst lane on an edge decides its colour.
    float edge_flow(u32 edge) const {
        float worst = 1.0f;
        const NetEdge& e = net_.edge(edge);
        for (u32 t = 0; t < static_cast<u32>(e.tracks.size()); ++t) {
            const u32 index = net_.track_index(TrackRef{edge, t});
            if (index < flow_.size()) worst = std::min(worst, flow_[index]);
        }
        return worst;
    }

    void draw_flow_overlay(Renderer2D& r) {
        if (!show_flow_) return;
        r.set_layer(0);
        for (u32 e = 0; e < static_cast<u32>(net_.edges().size()); ++e) {
            if (!net_.valid_edge(e)) continue;
            const float flow = edge_flow(e);
            const Color color = flow > 0.6f ? lerp_color(kFlowSlow, kFlowFree, clampf((flow - 0.6f) / 0.4f, 0, 1))
                                            : lerp_color(kFlowJam, kFlowSlow, clampf(flow / 0.6f, 0, 1));
            StrokeStyle band;
            band.width = net_.edge(e).width * 0.8f;
            band.join = LineJoin::Round;
            set_color(band, rgba(color.r, color.g, color.b, 150));
            r.draw_path(net_.edge(e).path, band);
        }
    }

    void arrive(u32 car, u64 tick) {
        ++arrived_;
        if (car >= car_info_.size()) return;
        const CarInfo& info = car_info_[car];
        if (info.route >= routes_.size()) return;
        RouteDemand& r = routes_[info.route];
        ++r.arrived;
        money_ += kFarePerTrip; // a served trip pays for itself
        const float minutes = static_cast<float>(tick - info.started) / 60.0f / 60.0f;
        r.last_minutes = minutes;
        r.best_minutes = r.best_minutes <= 0 ? minutes : std::min(r.best_minutes, minutes);
    }

    usize routes_connected() const {
        usize n = 0;
        for (const RouteDemand& r : routes_) n += r.connected ? 1 : 0;
        return n;
    }

    /// The network is about to change shape, so every route and every track
    /// index in it becomes stale. Remember where the cars physically are.
    struct Standing {
        Vec2 pos;
        float speed;
        CarInfo info;
    };

    std::vector<Standing> capture_cars() const {
        std::vector<Standing> standing;
        for (u32 i = 0; i < static_cast<u32>(cars_.agents().size()); ++i) {
            if (!cars_.valid(i) || i >= car_info_.size()) continue;
            standing.push_back(Standing{cars_.pose(net_, i).position, cars_.agent(i).speed, car_info_[i]});
        }
        return standing;
    }

    /// Put them back on the nearest lane and re-plan to where they were going.
    void restore_cars(const std::vector<Standing>& standing) {
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

    void rebuild_keeping_traffic() {
        auto standing = capture_cars();
        net_.rebuild();
        restore_cars(standing);
        refresh_connections();
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
        if (demo_) build_demo_roads();
        refresh_connections();
    }

    /// A few roads laid with the same tool the player uses, for screenshots
    /// and for seeing the junction geometry without drawing it by hand.
    void build_demo_roads() {
        const RoadType* street = roads_.find("street");
        const RoadType* avenue = roads_.find("avenue");
        const RoadType* motorway = roads_.find("highway");
        const auto spec_of = [](const RoadType* t) {
            return t ? traffic::RoadSpec{t->lanes_forward, t->lanes_backward, t->lane_width} : traffic::RoadSpec{};
        };
        const auto lay = [&](std::vector<Vec2> points, const RoadType* type) {
            traffic::build_road(net_, Path::from_catmull_rom(points), spec_of(type), 10.0f);
        };
        // A main road across the middle, two streets crossing it, and a
        // sweeping bypass so there is a curve to look at.
        lay({{-700, -40}, {-300, 10}, {120, 20}, {520, -30}, {760, -70}}, avenue);
        lay({{-260, -420}, {-240, -60}, {-230, 320}, {-260, 560}}, street);
        lay({{300, -520}, {320, -80}, {330, 300}, {380, 520}}, street);
        lay({{-640, -190}, {-500, -260}, {-260, -300}}, street);
        lay({{120, -640}, {140, -300}, {180, -60}}, street);
        lay({{700, -60}, {560, 180}, {330, 300}}, motorway);
        lay({{-260, 480}, {60, 520}, {390, 520}}, street);
        // Signals wherever four arms meet, so the demo shows them working.
        for (u32 n = 0; n < static_cast<u32>(net_.nodes().size()); ++n)
            if (net_.valid_node(n) && net_.edges_at(n).size() >= 4) toggle_signal(n);
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
        if (road_version_ != net_.version() || road_zoom_bucket_ != bucket || road_signal_version_ != signal_version_)
            rebuild_road_mesh(bucket);
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
        road_signal_version_ = signal_version_;
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

    /// European markings, all in real metres: kerb bands, edge lines, a centre
    /// line whose kind depends on the road, dashed lane dividers, and at every
    /// junction approach a stop line, a zebra crossing, give-way teeth for the
    /// minor road and a hatched keep-clear between opposing turn lanes.
    void build_edge_markings(const NetEdge& e) {
        const float total = e.path.length();
        const float head = end_clearance(e.a, e);
        const float tail = end_clearance(e.b, e);
        if (total - head - tail < 4) return;
        const Path body = e.path.sub_path(head, total - tail);
        if (body.empty()) return;
        const PathSampler along(body, 0.25f);
        if (along.empty()) return;
        const float half = e.width * 0.5f;

        std::vector<float> forward, backward;
        for (const Track& t : e.tracks) (t.forward ? forward : backward).push_back(t.offset);
        std::sort(forward.begin(), forward.end());
        std::sort(backward.begin(), backward.end());
        const usize per_side = std::max(forward.size(), backward.size());

        // Kerbs: from above a kerb is a pale band with its own shadow outside.
        StrokeStyle kerb;
        kerb.width = kKerbBand;
        set_color(kerb, kKerbTop);
        add_line(body.offset(half + kKerbBand * 0.5f), kerb);
        add_line(body.offset(-(half + kKerbBand * 0.5f)), kerb);
        StrokeStyle shadow;
        shadow.width = kKerbShadowBand;
        set_color(shadow, kKerbShadow);
        add_line(body.offset(half + kKerbBand + kKerbShadowBand * 0.5f), shadow);
        add_line(body.offset(-(half + kKerbBand + kKerbShadowBand * 0.5f)), shadow);

        // Edge lines, set in from the carriageway edge.
        StrokeStyle edge_line;
        edge_line.width = marking_width(kNarrowLine);
        set_color(edge_line, kMarkingWhite);
        add_line(body.offset(half - kEdgeLineInset), edge_line);
        add_line(body.offset(-(half - kEdgeLineInset)), edge_line);

        // Between the directions: dashed where overtaking is allowed, two solid
        // lines on anything wider.
        if (!forward.empty() && !backward.empty()) {
            const float at = (forward.front() + backward.back()) * 0.5f;
            StrokeStyle centre;
            centre.width = marking_width(kNarrowLine);
            set_color(centre, kMarkingWhite);
            if (per_side > 1) {
                add_line(body.offset(at - kNarrowLine), centre);
                add_line(body.offset(at + kNarrowLine), centre);
            } else {
                centre.dash = dash_mark(per_side);
                centre.gap = dash_gap(per_side);
                add_line(body.offset(at), centre);
            }
        }

        // Lane dividers within a direction.
        StrokeStyle lane;
        lane.width = marking_width(kNarrowLine);
        lane.dash = dash_mark(per_side);
        lane.gap = dash_gap(per_side);
        set_color(lane, kMarkingWhite);
        for (const std::vector<float>* side : {&forward, &backward})
            for (usize i = 1; i < side->size(); ++i) add_line(body.offset(((*side)[i - 1] + (*side)[i]) * 0.5f), lane);

        if (zoom_ < 3.0f) return; // the rest is detail nobody can see from up there
        build_approach(e, along, e.a, false, forward, backward);
        build_approach(e, along, e.b, true, forward, backward);
    }

    /// What a driver sees on the way into a junction: a stop line across the
    /// lanes going in, a zebra crossing a car length before it, give-way teeth
    /// when this is the minor road, and a hatched keep-clear between the
    /// opposing directions where the turn lanes would be.
    void build_approach(const NetEdge& e, const PathSampler& along, u32 node, bool at_end,
                        const std::vector<float>& forward, const std::vector<float>& backward) {
        if (!net_.valid_node(node)) return;
        const std::vector<u32> arms = net_.edges_at(node);
        if (arms.size() < 3) return; // a bend is not a junction

        // Lanes running into this end of the road are the ones that stop here.
        const std::vector<float>& incoming = at_end ? forward : backward;
        if (incoming.empty()) return;
        const float sign = at_end ? 1.0f : -1.0f;
        const float length = along.length();
        const float stop_at = at_end ? length - kStopLineSetback : kStopLineSetback;
        const float from = *std::min_element(incoming.begin(), incoming.end()) - e.width * 0.02f;
        const float to = *std::max_element(incoming.begin(), incoming.end()) + e.width * 0.02f;
        const float lane_half = e.width / std::max<float>(1, static_cast<float>(e.tracks.size())) * 0.5f;

        float widest = 0;
        for (u32 other : arms) widest = std::max(widest, net_.edge(other).width);
        const bool minor = e.width < widest - 0.5f;

        // A signalled approach stops at a line; otherwise the minor road gives
        // way and the road with priority is not marked at all.
        if (signal_at(node)) add_bar(along, stop_at, from - lane_half, to + lane_half, kStopLineWidth, kMarkingWhite);
        else if (minor) add_teeth(along, stop_at, from - lane_half, to + lane_half, sign);

        const float zebra_at = stop_at - sign * (kStopLineWidth + kZebraSetback + kZebraDepth * 0.5f);
        if (zebra_at > kZebraDepth && zebra_at < length - kZebraDepth) add_zebra(along, zebra_at, e.width);

        // Sperrflaeche: on a wide road the middle is kept clear where the
        // opposing turn lanes would otherwise run into each other.
        if (forward.size() > 1 && backward.size() > 1) {
            const float centre = (forward.front() + backward.back()) * 0.5f;
            const float d0 = at_end ? length - kHatchLength - kStopLineSetback : kStopLineSetback;
            add_hatched(along, d0, d0 + kHatchLength, centre - kNarrowLine * 3.0f, centre + kNarrowLine * 3.0f);
        }
    }

    /// A bar straight across the lanes: a stop line, or the base of anything else.
    void add_bar(const PathSampler& along, float at, float from, float to, float thickness, Color color) {
        if (at < 0 || at > along.length()) return;
        const PathPoint p = along.at(at);
        const Vec2 side = perp(p.direction);
        const Vec2 step = p.direction * (thickness * 0.5f);
        add_quad(p.position + side * from - step, p.position + side * to - step, p.position + side * to + step,
                 p.position + side * from + step, color);
    }

    /// A row of triangles pointing back at the driver who has to give way.
    void add_teeth(const PathSampler& along, float at, float from, float to, float sign) {
        if (at < 0 || at > along.length()) return;
        const PathPoint p = along.at(at);
        const Vec2 side = perp(p.direction);
        const Vec2 tip = p.direction * (-sign * kToothHeight);
        for (float x = from; x + kToothBase <= to; x += kToothBase + kToothGap) {
            const Vec2 a = p.position + side * x;
            const Vec2 b = p.position + side * (x + kToothBase);
            add_triangle(a, b, (a + b) * 0.5f + tip, kMarkingWhite);
        }
    }

    /// Bars along the road axis, the way a European zebra is painted.
    void add_zebra(const PathSampler& along, float at, float width) {
        const float half = width * 0.5f - kZebraInset;
        for (float x = -half; x + kZebraBar <= half; x += kZebraBar + kZebraGap) {
            const float centre = x + kZebraBar * 0.5f;
            add_strip(along, at - kZebraDepth * 0.5f, at + kZebraDepth * 0.5f, centre, kZebraBar, kMarkingWhite);
        }
    }

    /// A stripe running along the road at a lateral offset: zebra bars, hatch
    /// bars and anything else that follows the carriageway rather than crossing it.
    void add_strip(const PathSampler& along, float d0, float d1, float offset, float thickness, Color color) {
        d0 = clampf(d0, 0, along.length());
        d1 = clampf(d1, 0, along.length());
        if (d1 - d0 < 0.05f) return;
        const PathPoint a = along.at(d0);
        const PathPoint b = along.at(d1);
        const Vec2 sa = perp(a.direction) * offset;
        const Vec2 sb = perp(b.direction) * offset;
        const Vec2 ta = perp(a.direction) * (thickness * 0.5f);
        const Vec2 tb = perp(b.direction) * (thickness * 0.5f);
        add_quad(a.position + sa - ta, b.position + sb - tb, b.position + sb + tb, a.position + sa + ta, color);
    }

    /// A keep-clear area: a border line and diagonal bars inside it.
    void add_hatched(const PathSampler& along, float d0, float d1, float left, float right) {
        d0 = clampf(d0, 0, along.length());
        d1 = clampf(d1, 0, along.length());
        if (d1 - d0 < 2) return;
        const float centre = (left + right) * 0.5f;
        const float width = right - left;
        add_strip(along, d0, d1, left, kNarrowLine, kMarkingWhite);
        add_strip(along, d0, d1, right, kNarrowLine, kMarkingWhite);
        add_bar(along, d0, left, right, kNarrowLine, kMarkingWhite);
        add_bar(along, d1, left, right, kNarrowLine, kMarkingWhite);
        // Diagonals: a short bar walked along the area, offset sideways as it goes.
        for (float d = d0 + kHatchStep; d < d1 - 0.5f; d += kHatchStep) {
            const float back = std::max(d0 + 0.1f, d - width);
            const PathPoint a = along.at(d);
            const PathPoint b = along.at(back);
            const Vec2 pa = a.position + perp(a.direction) * (centre - width * 0.5f);
            const Vec2 pb = b.position + perp(b.direction) * (centre + width * 0.5f);
            const Vec2 dir = normalize(pb - pa);
            if (length_sq(dir) < 0.5f) continue;
            const Vec2 t = perp(dir) * (kHatchBar * 0.5f);
            add_quad(pa - t, pb - t, pb + t, pa + t, kMarkingWhite);
        }
    }

    void add_quad(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Color color) {
        const float r = color.r / 255.0f, g = color.g / 255.0f, bl = color.b / 255.0f, al = color.a / 255.0f;
        const u32 ia = marking_mesh_.add_vertex({a.x, a.y, 0}, {0, 0}, r, g, bl, al);
        const u32 ib = marking_mesh_.add_vertex({b.x, b.y, 0}, {1, 0}, r, g, bl, al);
        const u32 ic = marking_mesh_.add_vertex({c.x, c.y, 0}, {1, 1}, r, g, bl, al);
        const u32 id = marking_mesh_.add_vertex({d.x, d.y, 0}, {0, 1}, r, g, bl, al);
        marking_mesh_.add_quad(ia, ib, ic, id);
    }

    void add_triangle(Vec2 a, Vec2 b, Vec2 c, Color color) {
        const float r = color.r / 255.0f, g = color.g / 255.0f, bl = color.b / 255.0f, al = color.a / 255.0f;
        const u32 ia = marking_mesh_.add_vertex({a.x, a.y, 0}, {0, 0}, r, g, bl, al);
        const u32 ib = marking_mesh_.add_vertex({b.x, b.y, 0}, {1, 0}, r, g, bl, al);
        const u32 ic = marking_mesh_.add_vertex({c.x, c.y, 0}, {0.5f, 1}, r, g, bl, al);
        marking_mesh_.add_triangle(ia, ib, ic);
    }

    /// How much of an edge the junction at `node` eats.
    float end_clearance(u32 node, const NetEdge& e) const {
        if (!net_.valid_node(node)) return 0;
        return std::max(net_.node(node).radius, e.width * 0.55f);
    }

    /// Dash rhythms: shorter in town, longer the faster the road.
    static float dash_mark(usize lanes_per_side) { return lanes_per_side >= 3 ? 6.0f : lanes_per_side >= 2 ? 4.0f : 3.0f; }
    static float dash_gap(usize lanes_per_side) { return lanes_per_side >= 3 ? 12.0f : lanes_per_side >= 2 ? 8.0f : 3.0f; }

    void add_line(const std::vector<Vec2>& points, const StrokeStyle& style) {
        if (points.size() >= 2) build_polyline(points, style, marking_mesh_);
    }

    /// A car from directly above: a body with cut corners, a darker glasshouse,
    /// a lighter roof, and lamps that answer to what the car is doing.
    void draw_cars(Renderer2D& r) {
        const bool detail = zoom_ >= 4.0f;
        const bool blink = std::fmod(clock_, 0.9f) < 0.45f;
        r.set_layer(1);
        for (u32 i = 0; i < static_cast<u32>(cars_.agents().size()); ++i) {
            if (!cars_.valid(i)) continue;
            const NetAgent& a = cars_.agent(i);
            const PathPoint p = cars_.pose(net_, i);
            const VehicleType* type = a.tag < vehicles_.size() ? &vehicles_.at(a.tag) : nullptr;
            const float len = type ? type->length : a.params.length;
            const float wide = type ? type->width : 1.8f;
            const Vec2 dir = p.direction;
            const Vec2 side = perp(dir);
            const Color body = kCarColors[car_paint(i, type) % 16];

            const Vec2 front = p.position;
            const Vec2 back = front - dir * len;
            r.set_sort_key(front.y);
            // Shadow, on the same sun vector as the trees.
            const Vec2 drop = Vec2{0.35f, 0.45f};
            fill_car_body(r, front + drop, back + drop, side, wide, kVehicleShadow, 0.0f);
            fill_car_body(r, front, back, side, wide, body, 0.16f);
            if (!detail) continue;

            const Color glass = lerp_color(body, kGlassTint, 0.72f);
            const Color roof = lerp_color(body, rgb(255, 255, 255), 0.14f);
            // Bonnet, glasshouse, roof, boot, from the front backwards.
            const float cabin_start = len * 0.28f, cabin_end = len * 0.78f;
            fill_slab(r, front - dir * cabin_start, front - dir * (cabin_start + len * 0.1f), side, wide * 0.86f, kWindscreen);
            fill_slab(r, front - dir * (cabin_start + len * 0.1f), front - dir * cabin_end, side, wide * 0.9f, glass);
            fill_slab(r, front - dir * (len * 0.4f), front - dir * (len * 0.62f), side, wide * 0.62f, roof);

            // Lamps: red at the back under braking, amber on the side it is turning to.
            const bool braking = a.waiting || a.speed < 2.0f;
            const Color tail = braking ? kTailLampOn : kTailLampOff;
            const float lamp = std::max(0.16f, px(1.2f));
            const Vec2 corner = side * (wide * 0.36f);
            r.fill_circle(back.x + corner.x + dir.x * 0.2f, back.y + corner.y + dir.y * 0.2f, lamp, tail, 6);
            r.fill_circle(back.x - corner.x + dir.x * 0.2f, back.y - corner.y + dir.y * 0.2f, lamp, tail, 6);
            const int turn = turn_ahead(i);
            if (turn != 0 && blink) {
                const Vec2 at = front - dir * (len * 0.12f) + side * (wide * 0.36f * static_cast<float>(turn));
                r.fill_circle(at.x, at.y, lamp * 1.1f, kIndicatorOn, 6);
                const Vec2 rear = back + side * (wide * 0.36f * static_cast<float>(turn));
                r.fill_circle(rear.x, rear.y, lamp * 1.1f, kIndicatorOn, 6);
            }
        }
        r.set_sort_key(0);
        r.set_layer(0);
    }

    /// The body: a rectangle with its front corners cut, which from above is
    /// the difference between a car and a brick.
    void fill_car_body(Renderer2D& r, Vec2 front, Vec2 back, Vec2 side, float wide, Color color, float cut) {
        const Vec2 dir = normalize(front - back);
        const Vec2 half = side * (wide * 0.5f);
        const Vec2 nose = dir * (wide * cut);
        if (cut <= 0.001f) {
            r.fill_polygon({front + half, front - half, back - half, back + half}, color);
            return;
        }
        r.fill_polygon({front + half - nose, front + half * 0.55f, front - half * 0.55f, front - half - nose,
                        back - half, back - half * 0.75f + nose * 0.5f, back + half * 0.75f + nose * 0.5f, back + half},
                       color);
    }

    void fill_slab(Renderer2D& r, Vec2 a, Vec2 b, Vec2 side, float wide, Color color) {
        const Vec2 half = side * (wide * 0.5f);
        r.fill_polygon({a + half, a - half, b - half, b + half}, color);
    }

    /// Which way the car is about to turn: -1 left, 1 right, 0 straight on.
    /// Read out of the route, so the indicator can never lie.
    int turn_ahead(u32 car) const {
        const NetAgent& a = cars_.agent(car);
        if (a.step + 1 >= a.route.steps.size()) return 0;
        const RouteStep& now = a.route.steps[a.step];
        if (now.kind != RouteStep::Kind::Track) return 0;
        const float left = net_.track_length(net_.track_ref(now.index)) - a.distance;
        if (left > 30.0f) return 0;
        const RouteStep& next = a.route.steps[a.step + 1];
        if (next.kind != RouteStep::Kind::Connector || next.index >= net_.connectors().size()) return 0;
        const Connector& c = net_.connectors()[next.index];
        if (c.path.empty()) return 0;
        const Vec2 in = c.path.segments.front().direction(0);
        const Vec2 out = c.path.segments.back().direction(1);
        const float turn = cross(in, out);
        if (std::fabs(turn) < 0.25f) return 0;
        return turn > 0 ? 1 : -1; // y grows downwards, so a positive cross is a right turn
    }

    /// A car keeps its colour: it comes from the vehicle type and the slot,
    /// never from anything that changes while it drives.
    u32 car_paint(u32 car, const VehicleType* type) const {
        const u32 base = type ? type->color : 0;
        return base != 0 ? base : (cell_hash(static_cast<int>(car), 4242) >> 3) % 16;
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
        } else if (tool_ == kToolSignals) {
            for (const Signal& sig : signals_)
                if (net_.valid_node(sig.node))
                    r.fill_circle(net_.node(sig.node).pos.x, net_.node(sig.node).pos.y, net_.node(sig.node).radius,
                                  rgba(120, 200, 255, 45), 20);
            if (hover_node_ != kNone && net_.edges_at(hover_node_).size() >= 3) {
                const NetNode& n = net_.node(hover_node_);
                r.fill_circle(n.pos.x, n.pos.y, std::max(n.radius, px(8)), rgba(120, 200, 255, 110), 20);
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
        r.set_layer(-3); // under the roads: a road drawn through a village covers it
        for (usize i = 0; i < towns_.size(); ++i) {
            const Town& t = towns_.at(i);
            const Vec2 at = t.pos();
            const float radius = 18.0f + static_cast<float>(t.size) * 7.0f;
            // The ground a village stands on, then its houses: rectangles with
            // a roof ridge, laid out by hashing so a town always looks the same.
            r.fill_circle(at.x, at.y, radius, kTownGround, 24);
            if (zoom_ < 1.6f) continue; // from up here a village is a patch, not houses
            const u32 count = 6 + t.size * 4;
            for (u32 b = 0; b < count; ++b) {
                const u32 h = cell_hash(static_cast<int>(i) * 977 + static_cast<int>(b), 31337);
                const float angle = unit(h) * 6.2831853f;
                const float dist = std::sqrt(unit(h >> 7)) * radius * 0.86f;
                const Vec2 centre{at.x + std::cos(angle) * dist, at.y + std::sin(angle) * dist};
                const float w = 5.0f + unit(h >> 13) * 5.0f;
                const float d = 4.0f + unit(h >> 19) * 4.0f;
                const float facing = std::atan2(centre.y - at.y, centre.x - at.x);
                const Vec2 along{std::cos(facing), std::sin(facing)};
                const Vec2 side = perp(along);
                const Vec2 hw = along * (w * 0.5f), hd = side * (d * 0.5f);
                const Color roof = lerp_color(kRoofDark, kRoofLight, unit(h >> 24));
                r.fill_polygon({centre - hw - hd + Vec2{1.2f, 1.6f}, centre + hw - hd + Vec2{1.2f, 1.6f},
                                centre + hw + hd + Vec2{1.2f, 1.6f}, centre - hw + hd + Vec2{1.2f, 1.6f}},
                               kBuildingShadow);
                r.fill_polygon({centre - hw - hd, centre + hw - hd, centre + hw + hd, centre - hw + hd}, roof);
                // The ridge line down the middle of the roof.
                r.fill_polygon({centre - hw - hd * 0.16f, centre + hw - hd * 0.16f, centre + hw + hd * 0.16f,
                                centre - hw + hd * 0.16f},
                               lerp_color(roof, rgb(255, 240, 220), 0.25f));
            }
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

    /// A signal head at each approach, on the near side of the junction where
    /// the driver would look for it.
    void draw_signals(Renderer2D& r) {
        if (zoom_ < 2.0f) return;
        r.set_layer(2);
        for (const Signal& sig : signals_) {
            if (!net_.valid_node(sig.node)) continue;
            const NetNode& n = net_.node(sig.node);
            for (usize i = 0; i < sig.approach.size(); ++i) {
                const u32 index = sig.approach[i];
                if (!net_.valid_edge(index)) continue;
                const NetEdge& e = net_.edge(index);
                const Vec2 dir = e.a == sig.node ? e.path.segments.front().direction(0)
                                                 : e.path.segments.back().direction(1) * -1.0f;
                const Vec2 side = perp(dir);
                // Right-hand traffic: the head stands on the right of the lane
                // coming in, which is the left of the arm going out.
                const Vec2 at = n.pos + dir * (n.radius + 1.6f) - side * (e.width * 0.5f + 1.4f);
                const u8 state = sig.state_of(i);
                const Color lit = state == 2 ? kLightGreen : state == 1 ? kLightAmber : kLightRed;
                const float head = std::max(0.9f, px(3));
                r.fill_circle(at.x + 0.5f, at.y + 0.7f, head * 1.15f, rgba(20, 22, 20, 90), 10);
                r.fill_circle(at.x, at.y, head * 1.15f, kSignalPost, 10);
                r.fill_circle(at.x, at.y, head * 0.72f, lit, 10);
            }
        }
        r.set_layer(0);
    }

    void draw_hud(Renderer2D& r) {
        if (!app_) return;
        ui_.begin(&r, app_->ui_input());

        ui_.begin_panel(kToolPanel, "Build");
        bool build = tool_ == kToolBuild, bulldoze = tool_ == kToolBulldoze, signals = tool_ == kToolSignals;
        if (ui_.toggle("Road", build)) app_->push_command(kCmdTool, kToolBuild);
        ui_.same_line();
        if (ui_.toggle("Bulldoze", bulldoze)) app_->push_command(kCmdTool, kToolBulldoze);
        if (ui_.toggle("Traffic lights", signals)) app_->push_command(kCmdTool, kToolSignals);
        ui_.spacing(2);
        for (usize i = 0; i < roads_.size(); ++i) {
            const RoadType& type = roads_.at(i);
            char label[64];
            std::snprintf(label, sizeof label, "%s %ux%u  %.0f/m", type.id.c_str(), type.lanes_forward,
                          type.lanes_backward, type.cost_per_m);
            bool selected = type.id == road_type_.id;
            if (ui_.toggle(label, selected)) app_->push_command(kCmdRoad, static_cast<i32>(i));
        }
        ui_.end_panel();

        ui_.begin_panel(kInfoPanel, "Network");
        char line[80];
        std::snprintf(line, sizeof line, "%.0f of %.0f EUR", money_, kMoneyTarget);
        ui_.label(line);
        ui_.progress(clampf(money_ / kMoneyTarget, 0, 1), 186, {}, "");
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
        bool overlay = show_flow_;
        if (ui_.toggle("Traffic overlay", overlay)) app_->push_command(kCmdFlow);
        if (ui_.button("Clear traffic", 186)) app_->push_command(kCmdClearCars);
        ui_.end_panel();

        ui_.end();

        // What the road being drawn will cost, next to the cursor.
        if (drawing_ && stroke_.size() > 1) {
            const RoadType* type = road_type_.get(roads_);
            float drawn = 0;
            for (usize i = 1; i < stroke_.size(); ++i) drawn += distance(stroke_[i], stroke_[i - 1]);
            const float price = drawn * (type ? type->cost_per_m : 0);
            const Vec2 at = view(cam_center_, zoom_).to_screen(stroke_.back());
            char cost[64];
            std::snprintf(cost, sizeof cost, "%.0f m   %.0f EUR", drawn, price);
            r.draw_text(at.x + 14, at.y + 14, cost, price > money_ ? kBad : rgb(255, 240, 200), 2);
        }
        if (refusal_[0]) r.draw_text(12, kHeight - 60, refusal_, kBad, 2);
        r.draw_text(12, kHeight - 44,
                    tool_ == kToolBuild   ? "drag: draw a road   right click: cancel, drag to pan   wheel: zoom"
                    : tool_ == kToolSignals ? "click a junction to put traffic lights on it, or to take them away"
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
    float zoom_ = 2.2f;
    float zoom_target_ = 2.2f;
    std::vector<Signal> signals_; ///< kept sorted by node
    std::vector<TownAccess> access_;
    u64 signal_version_ = 0;      ///< bumped whenever a signal is added, removed or re-phased
    std::vector<RouteDemand> routes_;
    u64 access_version_ = ~0ull;
    std::vector<float> flow_; ///< per lane, 1 = free running, 0 = stopped
    float money_ = 20000;
    float spent_ = 0;
    bool show_flow_ = false;
    float clock_ = 0;    ///< seconds since the map started
    float demand_ = 3.0f; ///< trips per minute for a pair of size-1 towns
    u32 max_cars_ = 400;
    u32 spawned_ = 0;
    u32 arrived_ = 0;
    InputFrame prev_input_;

    // Derived per tick, not saved.
    u32 hover_node_ = kNone;
    u32 hover_edge_ = kNone;
    const char* refusal_ = "";
    float start_zoom_ = 2.2f;
    bool demo_ = false;
    bool start_flow_ = false;
    Vec2 start_at_{};
    bool has_start_at_ = false;
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
    u64 road_signal_version_ = ~0ull;
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
