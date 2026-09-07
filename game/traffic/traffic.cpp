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

#include "lanes.h"
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
const Color kRoofDark = rgb(104, 96, 88);
const Color kRoofLight = rgb(150, 128, 110);
const Color kBuildingShadow = rgba(40, 40, 34, 70);
/// European place-name plates: yellow for a village, blue for a main road
/// destination, green for a trunk road, white for anything local.
const Color kPlate[4] = {rgb(226, 190, 60), rgb(40, 82, 158), rgb(30, 110, 76), rgb(96, 100, 110)};
const Color kAutobahnBlue = rgb(26, 82, 158);
constexpr float kTownReach = 140.0f; ///< how far a town looks for a road to join
constexpr float kStartingMoney = 20000;
constexpr float kMoneyTarget = 60000;
constexpr float kFarePerTrip = 12;
const Color kAsphalt = rgb(58, 57, 57);
/// Markings are white in Europe, never yellow, and their sizes are real
/// metres so they stay right at every zoom.
const Color kMarkingWhite = rgba(232, 232, 226, 235);
constexpr float kNarrowLine = 0.12f;      ///< Schmalstrich
constexpr float kWideLine = 0.25f;        ///< Breitstrich
constexpr float kPocketLength = 45.0f;    ///< how far back a turn lane starts
constexpr float kPocketWidth = 3.0f;
constexpr float kTaperLength = 20.0f;     ///< how far the extra lane takes to open
constexpr float kMergeDots = 12.0f;       ///< dotted line leading into a merge
constexpr float kGuideDash = 0.5f;        ///< the dotted line through a junction
constexpr float kGuideGap = 0.5f;
const Color kGuideWhite = rgba(226, 226, 220, 180);
const Color kCycleSurface = rgb(150, 66, 52);
const Color kBusSurface = rgb(96, 74, 66);
const Color kParkingSurface = rgb(72, 71, 70);
const Color kShoulderSurface = rgb(78, 76, 72);
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
constexpr float kArrowLength = 5.0f;   ///< a 50 km/h arrow
constexpr float kArrowShaft = 0.50f;
constexpr float kArrowHead = 1.75f;
constexpr float kArrowHeadWidth = 1.50f;
constexpr float kArrowSetback = 8.0f; ///< tip this far before the stop line
constexpr float kArrowTurn = 1.30f;
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
enum Command : u32 {
    kCmdTool = 1,
    kCmdRoad = 2,
    kCmdClearCars = 3,
    kCmdFlow = 4,
    kCmdShape = 5,
    kCmdSnapAngle = 6,
    kCmdSnapGrid = 7,
};
enum Event : u32 { kEvBuilt = 1, kEvRemoved = 2 };

constexpr u32 kToolBuild = 0;
constexpr u32 kToolBulldoze = 1;
constexpr u32 kToolSignals = 2;
constexpr u32 kToolRoundabout = 3;
const char* const kToolNames[] = {"build", "bulldoze", "signals", "roundabout"};
constexpr float kRoundaboutRadius = 15.0f; ///< centreline; a 32 m inscribed circle

/// How a road is drawn. Straight and curve are click to click, the way a
/// road builder works; freehand is the pencil.
constexpr u32 kShapeStraight = 0;
constexpr u32 kShapeCurve = 1;
constexpr u32 kShapeSmooth = 2; ///< arcs that leave along the road they start from
constexpr u32 kShapeFree = 3;
const char* const kShapeNames[] = {"straight", "curve", "smooth", "freehand"};
constexpr float kMinRadius = 8.0f;    ///< an absolute floor for any road
constexpr float kLateralAccel = 2.5f; ///< what a comfortable bend allows, m/s^2
constexpr float kGridStep = 10.0f;   ///< metres, when snapping to the grid
constexpr float kAngleStep = 15.0f;  ///< degrees, when snapping the direction
const Color kIslandGrass = rgb(104, 128, 74);
const Color kIslandApron = rgb(150, 146, 138);
const Color kLightRed = rgb(226, 62, 52);
const Color kLightAmber = rgb(248, 178, 44);
const Color kLightGreen = rgb(86, 206, 96);
const Color kSignalPost = rgb(48, 50, 54);

// The shell: money at the top, the goals beside it, the build panel along
// the bottom, the minimap in the corner. The window is a fixed size, so
// these rectangles are constants and the simulation can decide by itself
// whether a click belonged to the HUD or to the world.
const Rect kMoneyPanel{kWidth * 0.5f - 150, 10, 300, 56};
const Rect kGoalPanel{kWidth - 274, 10, 262, 96};
const Rect kBuildPanel{kWidth * 0.5f - 448, kHeight - 132, 896, 120};
const Rect kMiniMap{12, kHeight - 184, 172, 172};
const Rect kToolStrip{kBuildPanel.x + 10, kBuildPanel.y + 22, 112, 92};
const Rect kShapeStrip{kBuildPanel.x + 130, kBuildPanel.y + 22, 216, 46};
constexpr float kCardWidth = 102;
constexpr float kCardHeight = 76;

inline bool over_hud(float x, float y) {
    return kMoneyPanel.contains(x, y) || kGoalPanel.contains(x, y) || kBuildPanel.contains(x, y) ||
           kMiniMap.contains(x, y);
}

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

using traffic::is_carriageway;
using traffic::kind_of;
using traffic::lane_kind_from;
using traffic::LaneKind;
using traffic::turn_sense;

/// One lane of a road type, in assets/road_types.json. Lanes are listed from
/// the left kerb to the right kerb, the way you would see them from above.
struct LaneSpec {
    std::string kind = "driving";
    float width = 3.25f;
    bool forward = true;

    void reflect(Visitor& v) {
        v.field("kind", kind);
        v.field("width", width, {.min = 0.8f, .max = 8});
        v.field("forward", forward);
    }
};

/// A kind of road, from assets/road_types.json.
struct RoadType {
    std::string id;
    u32 lanes_forward = 1;
    u32 lanes_backward = 1;
    float lane_width = 3.5f;
    float speed_kmh = 50;
    float cost_per_m = 2;         ///< what a metre of it costs to build
    std::vector<LaneSpec> lanes;  ///< when given, the road is exactly these lanes

    float width() const {
        if (lanes.empty()) return static_cast<float>(lanes_forward + lanes_backward) * lane_width;
        float total = 0;
        for (const LaneSpec& l : lanes) total += l.width;
        return total;
    }

    /// The lanes as the engine wants them: order and direction matter, the
    /// offsets are worked out when they are laid.
    std::vector<Track> tracks() const {
        std::vector<Track> out;
        if (lanes.empty()) {
            for (u32 i = 0; i < lanes_backward; ++i) out.push_back(Track{0, false, lane_width, 0});
            for (u32 i = 0; i < lanes_forward; ++i) out.push_back(Track{0, true, lane_width, 0});
            return out;
        }
        for (const LaneSpec& l : lanes)
            out.push_back(Track{0, l.forward, l.width, static_cast<u32>(lane_kind_from(l.kind))});
        return out;
    }

    void reflect(Visitor& v) {
        v.field("id", id);
        v.field("lanes_forward", lanes_forward, {.min = 0, .max = 5});
        v.field("lanes_backward", lanes_backward, {.min = 0, .max = 5});
        v.field("lane_width", lane_width, {.min = 1.5f, .max = 8});
        v.field("speed_kmh", speed_kmh, {.min = 10, .max = 160});
        v.field("cost_per_m", cost_per_m, {.min = 0, .max = 1000});
        v.field("lanes", lanes);
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

/// The middle of a roundabout: not a road, so the game remembers where its
/// islands are in order to draw them and to refuse building another on top.
struct Island {
    Vec2 centre;
    float radius = 15;

    void reflect(Visitor& v) {
        v.field("centre", centre);
        v.field("radius", radius, {.min = 4, .max = 80});
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
        net_.set_connect_filter(
            [](const CurveNetwork& net, u32, TrackRef from, TrackRef to) { return traffic::may_connect(net, from, to); });
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
        points_.clear();
        stroke_from_ = kNone;
        shape_ = kShapeStraight;
        tool_ = kToolBuild;
        road_type_ = DefRef<RoadType>{roads_.size() ? roads_.id_at(0) : std::string("street")};
        // Everything the starting map may fill in has to be empty first.
        money_ = kStartingMoney;
        spent_ = 0;
        flow_.clear();
        signals_.clear();
        islands_.clear();
        pockets_.clear();
        show_flow_ = start_flow_;
        demand_ = start_demand_;
        build_starting_map();
        cam_target_ = cam_center_ = has_start_at_ ? start_at_ : Vec2{0, 0};
        zoom_target_ = zoom_ = start_zoom_;
        clock_ = 0;
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
        apply_speed_limits();
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
        v.field("tool", tool_, {.names = kToolNames, .name_count = 4});
        v.field("islands", islands_);
        v.field("pockets", pockets_);
        v.field("auto_pockets", auto_pockets_);
        v.field("road_type", road_type_, {.table = "road_types"});
        v.field("stroke", stroke_);
        v.field("points", points_);
        v.field("shape", shape_, {.names = kShapeNames, .name_count = 4});
        v.field("entry_dir", entry_dir_);
        v.field("snap_angle", snap_angle_);
        v.field("snap_grid", snap_grid_);
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
        draw_islands(r);
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
        case kCmdTool: tool_ = static_cast<u32>(in.command_value) <= kToolRoundabout ? static_cast<u32>(in.command_value) : tool_; break;
        case kCmdRoad:
            if (in.command_value >= 0 && static_cast<usize>(in.command_value) < roads_.size())
                road_type_ = DefRef<RoadType>{roads_.id_at(static_cast<usize>(in.command_value))};
            break;
        case kCmdClearCars: clear_traffic(); break;
        case kCmdFlow: show_flow_ = !show_flow_; break;
        case kCmdShape:
            if (static_cast<u32>(in.command_value) <= kShapeFree) {
                entry_dir_ = Vec2{};
                shape_ = static_cast<u32>(in.command_value);
                points_.clear();
            }
            break;
        case kCmdSnapAngle: snap_angle_ = !snap_angle_; break;
        case kCmdSnapGrid: snap_grid_ = !snap_grid_; break;
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
        const bool on_hud = in.mouse_x < 0 || over_hud(static_cast<float>(in.mouse_x), static_cast<float>(in.mouse_y));
        const bool press = in.down(Button::MouseLeft) && !prev_input_.down(Button::MouseLeft);
        const bool release = !in.down(Button::MouseLeft) && prev_input_.down(Button::MouseLeft);
        const bool cancel = in.down(Button::MouseRight) && !prev_input_.down(Button::MouseRight);

        mouse_world_ = world;
        hover_node_ = net_.nearest_node(world, snap_range());
        hover_edge_ = kNone;
        if (hover_node_ == kNone) {
            const u32 e = net_.nearest_edge(world, 1e30f);
            if (e != kNone && distance(closest_on_edge(e, world), world) <= net_.edge(e).width * 0.5f + px(3)) hover_edge_ = e;
        }

        if (tool_ == kToolBuild && shape_ != kShapeFree) {
            // Click to click, the way a road is really laid out: one click for
            // the start, one for the end, and for a curve one more in between
            // that the road bends around. The end becomes the start of the
            // next piece, so a route is drawn as a chain.
            if (cancel) {
                points_.clear();
                entry_dir_ = Vec2{};
            } else if (press && !on_hud) {
                const Vec2 at = snap_point(world);
                if (points_.empty() && shape_ == kShapeSmooth)
                    entry_dir_ = hover_node_ != kNone ? tangent_at(hover_node_, world) : Vec2{};
                points_.push_back(at);
                const usize needed = shape_ == kShapeCurve ? 3u : 2u;
                if (points_.size() >= needed) {
                    const Path path = drawn_path(points_);
                    const Vec2 last = points_.back();
                    if (commit_path(ctx, path)) {
                        points_ = {last};
                        // A smooth chain carries on the way the last piece ended.
                        if (shape_ == kShapeSmooth && !path.empty())
                            entry_dir_ = path.segments.back().direction(1);
                    } else {
                        points_.clear();
                    }
                }
            }
        } else if (tool_ == kToolBuild) {
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
                const Path path = fit_path(simplify_polyline(stroke_, 0.4f), 1.2f);
                commit_path(ctx, path);
                drawing_ = false;
                stroke_.clear();
                stroke_from_ = kNone;
            }
        } else if (tool_ == kToolRoundabout) {
            if (release && !on_hud) place_roundabout(ctx, world);
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

    /// Where a click actually lands: an existing junction, a point on an
    /// existing road, a grid corner, or a direction locked to a step from the
    /// point before it. Snapping is what makes click-to-click building feel
    /// like a tool rather than a sketch.
    Vec2 snap_point(Vec2 world) const {
        if (hover_node_ != kNone) return net_.node(hover_node_).pos;
        if (hover_edge_ != kNone) {
            const PathSampler s(net_.edge(hover_edge_).path, 0.25f);
            if (!s.empty()) return s.closest(world).position;
        }
        Vec2 at = world;
        if (!points_.empty() && snap_angle_) {
            const Vec2 from = points_.back();
            const Vec2 delta = at - from;
            const float len = length(delta);
            if (len > 0.5f) {
                const float step = kAngleStep * 3.14159265f / 180.0f;
                const float snapped = std::round(angle_of(delta) / step) * step;
                at = from + Vec2{std::cos(snapped), std::sin(snapped)} * len;
            }
        }
        if (snap_grid_) at = {std::round(at.x / kGridStep) * kGridStep, std::round(at.y / kGridStep) * kGridStep};
        return at;
    }

    /// Two points make a straight, three make a curve bending around the middle one.
    static Path shape_path(const std::vector<Vec2>& points) {
        Path path;
        if (points.size() < 2) return path;
        path.move_to(points[0]);
        if (points.size() == 2) path.line_to(points[1]);
        else path.quad_to(points[1], points[2]);
        return path;
    }

    /// Which way a road leaves this junction, chosen as the arm pointing most
    /// nearly at where the player is heading.
    Vec2 tangent_at(u32 node, Vec2 towards) const {
        if (!net_.valid_node(node)) return Vec2{};
        Vec2 best{};
        float best_dot = -2;
        const Vec2 wanted = normalize(towards - net_.node(node).pos);
        for (u32 e : net_.edges_at(node)) {
            const NetEdge& edge = net_.edge(e);
            if (edge.path.empty()) continue;
            // The direction the road runs, pointing away from this node, then
            // reversed: a road carries on the way it came in.
            // The direction this road runs *away* from the node, reversed: a
            // road carries on the way it came in, it does not double back.
            const Vec2 out = edge.a == node ? edge.path.segments.front().direction(0)
                                            : edge.path.segments.back().direction(1) * -1.0f;
            const Vec2 carry = out * -1.0f;
            const float d = dot(carry, wanted);
            if (d > best_dot) {
                best_dot = d;
                best = carry;
            }
        }
        return best;
    }

    /// The radius below which this road is uncomfortable at its own speed.
    /// Above the hard floor this only warns: real streets are full of bends
    /// tighter than the design speed, and a builder that forbids them is a
    /// builder the player fights.
    float comfortable_radius(const RoadType* type) const {
        const float speed = (type ? type->speed_kmh : 50.0f) / 3.6f;
        return std::max(kMinRadius, speed * speed / kLateralAccel);
    }

    /// The radius below which the lanes themselves would fold over: half the
    /// road plus a margin, because every lane is an offset of the centreline.
    static float hard_radius(float road_width) { return std::max(kMinRadius, road_width * 0.5f + 2.0f); }

    /// Whatever the current shape makes of these points.
    Path drawn_path(const std::vector<Vec2>& points, float* radius = nullptr) const {
        if (shape_ == kShapeSmooth && points.size() >= 2 && length_sq(entry_dir_) > 0.1f)
            return traffic::arc_between(points[0], entry_dir_, points[1], radius);
        return shape_path(points);
    }

    /// The road that is being drawn right now, including where the mouse is.
    Path preview_path(float* radius = nullptr) const {
        std::vector<Vec2> points = points_;
        if (points.empty()) return Path{};
        points.push_back(snap_point(mouse_world_));
        return drawn_path(points, radius);
    }

    /// Lay a road along `path`. Everything it crosses is split, so a line
    /// drawn over another road becomes a junction rather than a bridge.
    bool commit_path(const TickContext& ctx, const Path& path) {
        const RoadType* type = road_type_.get(roads_);
        if (!type || path.empty()) return false;

        float tightest = 0;
        for (const CubicBezier& seg : path.segments) {
            for (int i = 0; i <= 8; ++i) {
                const float k = std::fabs(seg.curvature(static_cast<float>(i) / 8));
                if (k > 1e-6f) tightest = std::max(tightest, k);
            }
        }
        if (tightest > 0 && 1.0f / tightest < hard_radius(type->width())) {
            refusal_ = "bend too tight for this road";
            return false;
        }
        const float price = path.length() * type->cost_per_m;
        if (price > money_) {
            refusal_ = "not enough money";
            return false;
        }
        const traffic::RoadSpec spec = spec_for(*type);
        auto standing = capture_cars(); // build_road rebuilds, which invalidates every route
        const traffic::BuildResult built = traffic::build_road(net_, path, spec, snap_range());
        if (!built.ok) {
            refusal_ = built.refusal;
            restore_cars(standing);
            return false;
        }
        refusal_ = "";
        money_ -= built.length * type->cost_per_m;
        spent_ += built.length * type->cost_per_m;
        if (auto_pockets_) ensure_pockets(built.junctions);
        restore_cars(standing);
        refresh_connections();
        const Vec2 mid = path.sub_path(0, path.length() * 0.5f).end();
        ctx.emit(kEvBuilt, mid.x, mid.y);
        return true;
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
        apply_route_penalties(); // this moves the version, so stamp it afterwards
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
            const Color color = flow_color(edge_flow(e));
            StrokeStyle band;
            band.width = net_.edge(e).width * 0.8f;
            band.join = LineJoin::Round;
            set_color(band, rgba(color.r, color.g, color.b, 150));
            r.draw_path(net_.edge(e).path, band);
        }
    }

    // ---- speed limits ---------------------------------------------------------

    /// Everything the builder needs to lay this kind of road.
    traffic::RoadSpec spec_for(const RoadType& type) const {
        traffic::RoadSpec spec;
        spec.lanes_forward = type.lanes_forward;
        spec.lanes_backward = type.lanes_backward;
        spec.lane_width = type.lane_width;
        spec.lanes = type.tracks();
        const i32 index = roads_.index_of(type.id);
        spec.tag = index < 0 ? 0 : static_cast<u32>(index);
        return spec;
    }

    const RoadType* road_of(u32 edge) const {
        if (!net_.valid_edge(edge)) return nullptr;
        const u32 tag = net_.edge(edge).tag;
        return tag < roads_.size() ? &roads_.at(tag) : nullptr;
    }

    float limit_of(u32 edge) const {
        const RoadType* type = road_of(edge);
        return (type ? type->speed_kmh : 50.0f) / 3.6f;
    }

    /// Every car drives at the lower of what it can do and what the road it is
    /// on allows, so a motorway really is faster than a lane.
    void apply_speed_limits() {
        for (u32 i = 0; i < static_cast<u32>(cars_.agents().size()); ++i) {
            if (!cars_.valid(i)) continue;
            NetAgent& a = cars_.agent(i);
            const RouteStep* step = cars_.current_step(i);
            if (!step) continue;
            u32 edge = kNone;
            if (step->kind == RouteStep::Kind::Connector && step->index < net_.connectors().size())
                edge = net_.connectors()[step->index].from.edge;
            else if (step->index < net_.track_count())
                edge = net_.track_ref(step->index).edge;
            if (edge == kNone) continue;
            const float top = a.tag < vehicles_.size() ? vehicles_.at(a.tag).top_speed_kmh / 3.6f : 25.0f;
            a.params.max_speed = std::min(top, limit_of(edge));
        }
    }

    /// Routing costs are lengths, so a slow road is made to look longer than
    /// it is: the cars then pick the quicker way rather than the shorter one.
    void apply_route_penalties() {
        net_.clear_track_penalties();
        constexpr float kReference = 50.0f / 3.6f;
        for (u32 e = 0; e < static_cast<u32>(net_.edges().size()); ++e) {
            if (!net_.valid_edge(e)) continue;
            const float speed = std::max(limit_of(e), 4.0f);
            const float factor = kReference / speed - 1.0f;
            if (std::fabs(factor) < 0.02f) continue;
            for (u32 t = 0; t < static_cast<u32>(net_.edge(e).tracks.size()); ++t) {
                const TrackRef track{e, t};
                net_.set_track_penalty(track, net_.track_length(track) * factor);
            }
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

    /// A roundabout: a ring of one-way road, and an island in the middle that
    /// keeps its own record because it is not part of the network.
    void place_roundabout(const TickContext& ctx, Vec2 at) {
        const RoadType* ring = roads_.find("roundabout");
        if (!ring) {
            refusal_ = "no roundabout road type";
            return;
        }
        for (const Island& island : islands_) {
            if (distance(island.centre, at) < island.radius + kRoundaboutRadius) {
                refusal_ = "too close to another roundabout";
                return;
            }
        }
        const float price = 6.2831853f * kRoundaboutRadius * ring->cost_per_m;
        if (price > money_) {
            refusal_ = "not enough money";
            return;
        }
        const traffic::RoadSpec spec = spec_for(*ring);
        auto standing = capture_cars();
        const traffic::BuildResult built = traffic::build_roundabout(net_, at, kRoundaboutRadius, spec, snap_range());
        if (!built.ok) {
            refusal_ = built.refusal;
            restore_cars(standing);
            return;
        }
        refusal_ = "";
        money_ -= price;
        spent_ += price;
        islands_.push_back(Island{at, kRoundaboutRadius});
        restore_cars(standing);
        refresh_connections();
        ctx.emit(kEvBuilt, at.x, at.y);
    }

    // ---- turn pockets ----------------------------------------------------------
    //
    // A junction on a wide road gets dedicated turn lanes: the approach is cut
    // a pocket length before the junction and the short piece gains a lane
    // that only turners may use. The connect filter then keeps the through
    // traffic out of it and the arrows follow, because they read the same
    // connectors.

    /// Does this edge already carry a pocket for that direction?
    static bool has_pocket(const NetEdge& e, bool forward) {
        for (const Track& t : e.tracks) {
            if (t.forward != forward) continue;
            const LaneKind kind = kind_of(t);
            if (kind == LaneKind::LeftTurn || kind == LaneKind::RightTurn) return true;
        }
        return false;
    }

    static u32 driving_lanes(const NetEdge& e, bool forward) {
        u32 n = 0;
        for (const Track& t : e.tracks)
            if (t.forward == forward && kind_of(t) == LaneKind::Driving) ++n;
        return n;
    }

    /// Insert a turn lane into an edge, on the side the given direction turns
    /// towards. Lanes run left to right, so the slot next to the centre line
    /// is where both directions put their left turn.
    ///
    /// The lanes of the direction that gains the pocket keep the offsets they
    /// had: the road grows on the other side of the new lane. Re-centring the
    /// whole cross-section instead would slide every through lane sideways by
    /// half a lane, and the connectors at the taper would become dog-legs.
    static void insert_pocket(NetEdge& e, bool forward, bool left, float width) {
        usize backward_count = 0;
        for (const Track& t : e.tracks)
            if (!t.forward) ++backward_count;
        const usize at = forward ? (left ? backward_count : e.tracks.size()) : (left ? backward_count : 0);
        if (e.tracks.empty()) return;

        float offset = 0;
        if (at == 0) {
            offset = e.tracks.front().offset - e.tracks.front().width * 0.5f - width * 0.5f;
        } else if (at >= e.tracks.size()) {
            offset = e.tracks.back().offset + e.tracks.back().width * 0.5f + width * 0.5f;
        } else if (forward) {
            // Grows towards the centre: the lanes to the left of it move over.
            const Track& next = e.tracks[at];
            offset = next.offset - next.width * 0.5f - width * 0.5f;
            for (usize i = 0; i < at; ++i) e.tracks[i].offset -= width;
        } else {
            const Track& previous = e.tracks[at - 1];
            offset = previous.offset + previous.width * 0.5f + width * 0.5f;
            for (usize i = at; i < e.tracks.size(); ++i) e.tracks[i].offset += width;
        }
        Track lane{offset, forward, width, static_cast<u32>(left ? LaneKind::LeftTurn : LaneKind::RightTurn)};
        e.tracks.insert(e.tracks.begin() + static_cast<std::ptrdiff_t>(at), lane);
        e.width += width;
    }

    /// Give one approach of a junction its pocket. Returns whether anything
    /// changed; the caller rebuilds once for all of them.
    bool add_pocket(u32 node, u32 edge, bool left) {
        if (!net_.valid_edge(edge) || !net_.valid_node(node)) return false;
        const NetEdge& e = net_.edge(edge);
        const bool incoming_forward = e.b == node; // forward lanes run from a to b
        if (e.a != node && e.b != node) return false;
        if (has_pocket(e, incoming_forward)) return false;
        if (driving_lanes(e, incoming_forward) < 2) return false;
        const float total = e.path.length();
        if (total < kPocketLength * 2.2f) return false;

        const float cut = incoming_forward ? total - kPocketLength : kPocketLength;
        const u32 mid = traffic::split_at(net_, edge, cut, kTaperLength * 0.5f);
        if (mid == kNone) return false;
        // split_edge keeps the first half at the old index and appends the tail.
        const u32 pocket = incoming_forward ? static_cast<u32>(net_.edges().size()) - 1 : edge;
        if (!net_.valid_edge(pocket)) return false;
        insert_pocket(net_.edge(pocket), incoming_forward, left, kPocketWidth);
        pockets_.push_back(pocket);
        return true;
    }

    /// Wide approaches to a proper crossroads get a left turn pocket, which is
    /// the one that pays for itself: it takes the turners out of the through
    /// lane instead of letting them block it.
    void ensure_pockets(const std::vector<u32>& nodes) {
        bool changed = false;
        for (u32 node : nodes) {
            if (!net_.valid_node(node)) continue;
            const std::vector<u32> arms = net_.edges_at(node);
            if (arms.size() < 4) continue;
            for (u32 edge : arms) changed |= add_pocket(node, edge, true);
        }
        if (changed) net_.rebuild();
    }

    bool is_ring(u32 edge) const {
        const RoadType* type = road_of(edge);
        return type && type->id == "roundabout";
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
            const u32 edge = net_.add_edge(a, b);
            if (type) spec_for(*type).apply(net_, edge);
            else net_.set_tracks(edge, lanes_f, lanes_b, lane_w);
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
        const auto spec_of = [this](const RoadType* t) { return t ? spec_for(*t) : traffic::RoadSpec{}; };
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
        // A roundabout where the eastern street meets the main road.
        if (const RoadType* ring = roads_.find("roundabout")) {
            traffic::RoadSpec spec = spec_of(ring);
            const Vec2 at{325, 5};
            if (traffic::build_roundabout(net_, at, kRoundaboutRadius, spec, 10.0f).ok)
                islands_.push_back(Island{at, kRoundaboutRadius});
        }
        // Turn pockets wherever a wide road meets a crossroads.
        if (auto_pockets_) {
            std::vector<u32> nodes;
            for (u32 n = 0; n < static_cast<u32>(net_.nodes().size()); ++n)
                if (net_.valid_node(n)) nodes.push_back(n);
            ensure_pockets(nodes);
        }
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
        build_junction_guides();
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
        const usize arm_count = net_.edges_at(node).size();
        if (arm_count < 3) return; // a joint between two pieces of one road needs no paving
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

        // Lanes that are not for cars get their own surface, the way a bus
        // lane or a cycle track reads differently from the carriageway.
        for (const Track& t : e.tracks) {
            const LaneKind kind = kind_of(t);
            if (kind == LaneKind::Driving || kind == LaneKind::LeftTurn || kind == LaneKind::RightTurn) continue;
            const Color surface = kind == LaneKind::Cycle    ? kCycleSurface
                                  : kind == LaneKind::Bus    ? kBusSurface
                                  : kind == LaneKind::Parking ? kParkingSurface
                                                              : kShoulderSurface;
            StrokeStyle strip;
            strip.width = t.width - 0.1f;
            set_color(strip, surface);
            add_line(body.offset(t.offset), strip);
            // A wide dashed line separates a bus lane from the traffic.
            if (kind == LaneKind::Bus) {
                StrokeStyle border;
                border.width = marking_width(kWideLine);
                border.dash = 3.0f;
                border.gap = 3.0f;
                set_color(border, kMarkingWhite);
                add_line(body.offset(t.offset - (t.forward ? t.width * 0.5f : -t.width * 0.5f)), border);
            }
        }

        if (zoom_ < 3.0f) return; // the rest is detail nobody can see from up there
        build_tapers(e, edge_index(e), along);
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
        bool ring_here = false;
        for (u32 other : arms) {
            widest = std::max(widest, net_.edge(other).width);
            ring_here = ring_here || is_ring(other);
        }
        // Everything gives way to a roundabout; otherwise the narrower road does.
        const bool minor = ring_here ? !is_ring(edge_index(e)) : e.width < widest - 0.5f;
        if (ring_here && is_ring(edge_index(e))) return; // the ring itself is not marked at its entries

        // A signalled approach stops at a line; otherwise the minor road gives
        // way and the road with priority is not marked at all.
        if (signal_at(node)) add_bar(along, stop_at, from - lane_half, to + lane_half, kStopLineWidth, kMarkingWhite);
        else if (minor) add_teeth(along, stop_at, from - lane_half, to + lane_half, sign);

        const float zebra_at = stop_at - sign * (kStopLineWidth + kZebraSetback + kZebraDepth * 0.5f);
        if (zebra_at > kZebraDepth && zebra_at < length - kZebraDepth) add_zebra(along, zebra_at, e.width);

        // One arrow per lane, showing where that lane may go. The turns come
        // from the junction's own connectors, so an arrow cannot lie.
        const float arrow_at = stop_at - sign * (kArrowSetback + kArrowLength);
        if (zoom_ >= 5.0f && arrow_at > kArrowLength && arrow_at < length - kArrowLength) {
            for (u32 t = 0; t < static_cast<u32>(e.tracks.size()); ++t) {
                const Track& track = e.tracks[t];
                if (track.forward != at_end) continue; // only the lanes running into this junction
                add_arrow(along, arrow_at, track.offset, turns_from(TrackRef{edge_index(e), t}), sign);
            }
        }

        // Sperrflaeche: on a wide road the middle is kept clear where the
        // opposing turn lanes would otherwise run into each other.
        if (forward.size() > 1 && backward.size() > 1) {
            const float centre = (forward.front() + backward.back()) * 0.5f;
            const float d0 = at_end ? length - kHatchLength - kStopLineSetback : kStopLineSetback;
            add_hatched(along, d0, d0 + kHatchLength, centre - kNarrowLine * 3.0f, centre + kNarrowLine * 3.0f);
        }
    }

    /// Which ways a lane may go from here: bit 1 left, 2 straight, 4 right.
    u32 turns_from(TrackRef track) const {
        u32 turns = 0;
        for (u32 c : net_.connectors_from(track)) {
            const Connector& conn = net_.connectors()[c];
            if (conn.path.empty()) continue;
            const float turn = cross(conn.path.segments.front().direction(0), conn.path.segments.back().direction(1));
            if (turn > 0.25f) turns |= 4;
            else if (turn < -0.25f) turns |= 1;
            else turns |= 2;
        }
        return turns ? turns : 2;
    }

    /// A painted arrow: a shaft with a head, plus a barb to the side for each
    /// turn the lane allows.
    void add_arrow(const PathSampler& along, float at, float offset, u32 turns, float sign) {
        if (at < 0 || at > along.length()) return;
        const PathPoint p = along.at(at);
        const Vec2 forward = p.direction * sign;
        const Vec2 side = perp(p.direction) * sign;
        const Vec2 base = p.position + side * offset - forward * (kArrowLength * 0.5f);
        const Vec2 tip = base + forward * kArrowLength;
        const bool straight = (turns & 2) != 0;

        const Vec2 shaft_half = side * (kArrowShaft * 0.5f);
        const Vec2 shaft_end = straight ? tip - forward * kArrowHead : base + forward * (kArrowLength * 0.55f);
        add_quad(base - shaft_half, shaft_end - shaft_half, shaft_end + shaft_half, base + shaft_half, kMarkingWhite);
        if (straight) add_triangle(tip, shaft_end - side * (kArrowHeadWidth * 0.5f), shaft_end + side * (kArrowHeadWidth * 0.5f),
                                   kMarkingWhite);

        for (int direction = -1; direction <= 1; direction += 2) {
            const u32 bit = direction < 0 ? 1u : 4u;
            if ((turns & bit) == 0) continue;
            const Vec2 out = side * (static_cast<float>(direction) * kArrowTurn);
            const Vec2 corner = shaft_end + forward * (kArrowLength * 0.12f);
            const Vec2 end = corner + out;
            const Vec2 thick = forward * (kArrowShaft * 0.5f);
            add_quad(corner - thick, end - thick, end + thick, corner + thick, kMarkingWhite);
            const Vec2 head = end + out * 0.55f;
            add_triangle(head, end - forward * (kArrowHeadWidth * 0.5f), end + forward * (kArrowHeadWidth * 0.5f),
                         kMarkingWhite);
        }
    }

    u32 edge_index(const NetEdge& e) const { return static_cast<u32>(&e - net_.edges().data()); }

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

    /// How much of an edge the junction at `node` eats. A joint between two
    /// pieces of the same road eats almost nothing, so the markings run on.
    float end_clearance(u32 node, const NetEdge& e) const {
        if (!net_.valid_node(node)) return 0;
        if (net_.edges_at(node).size() < 3) return std::min(net_.node(node).radius, 1.0f);
        return std::max(net_.node(node).radius, e.width * 0.55f);
    }

    /// Dash rhythms: shorter in town, longer the faster the road.
    static float dash_mark(usize lanes_per_side) { return lanes_per_side >= 3 ? 6.0f : lanes_per_side >= 2 ? 4.0f : 3.0f; }
    static float dash_gap(usize lanes_per_side) { return lanes_per_side >= 3 ? 12.0f : lanes_per_side >= 2 ? 8.0f : 3.0f; }

    /// A lane that nothing feeds opens over a taper, and one that carries on
    /// nowhere closes over one. That covers a turn pocket, a road that gains a
    /// lane and a road that loses one, without any of them being a special case.
    void build_tapers(const NetEdge& e, u32 index, const PathSampler& along) {
        const float length = along.length();
        if (length < kTaperLength + 6) return;
        for (u32 t = 0; t < static_cast<u32>(e.tracks.size()); ++t) {
            const Track& lane = e.tracks[t];
            if (!is_carriageway(kind_of(lane))) continue;
            const TrackRef ref{index, t};
            // A forward lane runs from the start of the edge to its end.
            if (!traffic::lane_is_fed(net_, ref)) add_taper(e, along, lane, lane.forward ? 0.0f : length, true);
            if (!traffic::lane_continues(net_, ref)) add_taper(e, along, lane, lane.forward ? length : 0.0f, false);
        }
    }

    /// One taper: the lane's edge angles across its own width, opening away
    /// from `at` or closing towards it.
    void add_taper(const NetEdge& e, const PathSampler& along, const Track& lane, float at, bool opening) {
        const float length = along.length();
        // The lane closes towards whichever side its neighbours are on.
        float inward = 0;
        for (const Track& other : e.tracks) {
            if (&other == &lane || other.forward != lane.forward || !is_carriageway(kind_of(other))) continue;
            inward += other.offset > lane.offset ? 1.0f : -1.0f;
        }
        if (inward == 0) inward = lane.forward ? -1.0f : 1.0f; // alone: close towards the centre
        inward = inward > 0 ? 1.0f : -1.0f;

        const float travel = lane.forward ? 1.0f : -1.0f;
        const float far = opening ? at + travel * kTaperLength : at - travel * kTaperLength;
        const float from = clampf(std::min(at, far), 0, length);
        const float to = clampf(std::max(at, far), 0, length);
        if (to - from < 2) return;
        const bool start_is_narrow = opening ? (at < far) == (travel > 0) : (at < far) == (travel < 0);
        const PathPoint a = along.at(from);
        const PathPoint b = along.at(to);
        const float narrow = lane.offset + inward * lane.width * 0.5f;
        const float wide = lane.offset - inward * lane.width * 0.5f;
        const Vec2 pa = a.position + perp(a.direction) * (start_is_narrow ? narrow : wide);
        const Vec2 pb = b.position + perp(b.direction) * (start_is_narrow ? wide : narrow);
        const Vec2 dir = normalize(pb - pa);
        if (length_sq(dir) < 0.5f) return;
        const Vec2 thick = perp(dir) * (marking_width(kNarrowLine) * 0.5f);
        add_quad(pa - thick, pb - thick, pb + thick, pa + thick, kMarkingWhite);

        // Merging traffic gets a dotted line rather than a solid one.
        StrokeStyle dotted;
        dotted.width = marking_width(kNarrowLine);
        dotted.dash = 1.0f;
        dotted.gap = 1.0f;
        set_color(dotted, kGuideWhite);
        const Path body = e.path.sub_path(std::max(0.0f, from - kMergeDots), from);
        if (!body.empty()) add_line(body.offset(lane.offset + inward * lane.width * 0.5f), dotted);
    }

    /// The dotted guide lines that carry a lane across a junction, so a
    /// turning driver can see where they are meant to come out.
    void build_junction_guides() {
        if (zoom_ < 5.0f) return;
        StrokeStyle guide;
        guide.width = marking_width(kNarrowLine);
        guide.dash = kGuideDash;
        guide.gap = kGuideGap;
        set_color(guide, kGuideWhite);
        for (const Connector& c : net_.connectors()) {
            if (c.path.empty() || c.length < 4) continue;
            // Only the left turn is guided across the junction: it is the
            // movement that crosses oncoming traffic and the one drivers get
            // wrong. Guiding everything turns a junction into spaghetti.
            if (turn_sense(net_, c.from, c.to) >= 0) continue;
            std::vector<Vec2> line;
            c.path.flatten(0.3f, line);
            add_line(line, guide);
        }
    }

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
        if (tool_ == kToolBuild && shape_ != kShapeFree) {
            const RoadType* type = road_type_.get(roads_);
            const Path ghost = preview_path();
            if (!ghost.empty()) {
                std::vector<Vec2> line;
                ghost.flatten(0.4f, line);
                StrokeStyle road;
                road.width = type ? type->width() : 7;
                road.join = LineJoin::Round;
                road.cap = LineCap::Butt;
                set_color(road, rgba(kAsphalt.r, kAsphalt.g, kAsphalt.b, 150));
                r.draw_polyline(line, road);
                StrokeStyle edge;
                edge.width = std::max(0.3f, px(1.5f));
                set_color(edge, kGhost);
                r.draw_polyline(line, edge);
            }
            for (const Vec2& p : points_) r.fill_circle(p.x, p.y, std::max(0.8f, px(4)), kGhost, 12);
            const Vec2 at = snap_point(mouse_world_);
            r.fill_circle(at.x, at.y, std::max(0.6f, px(3)), rgba(255, 255, 255, 180), 10);
            if (snap_grid_) draw_grid(r);
        } else if (tool_ == kToolBuild) {
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
        } else if (tool_ == kToolRoundabout) {
            StrokeStyle ring;
            ring.width = 6.5f;
            ring.closed = true;
            set_color(ring, rgba(255, 214, 90, 120));
            std::vector<Vec2> circle;
            for (int i = 0; i < 32; ++i) {
                const float a = static_cast<float>(i) / 32 * 6.2831853f;
                circle.push_back({mouse_world_.x + std::cos(a) * kRoundaboutRadius,
                                  mouse_world_.y + std::sin(a) * kRoundaboutRadius});
            }
            r.draw_polyline(circle, ring);
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

    /// The middle of a roundabout: an overrun apron the lorries can clip, and
    /// grass inside it.
    void draw_islands(Renderer2D& r) {
        r.set_layer(-1);
        for (const Island& island : islands_) {
            const float inner = island.radius - 4.0f;
            if (inner < 2) continue;
            r.fill_circle(island.centre.x, island.centre.y, inner, kIslandApron, 28);
            r.fill_circle(island.centre.x, island.centre.y, inner - 2.0f, kIslandGrass, 28);
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
            r.fill_circle(at.x, at.y, radius, rgba(kTownGround.r, kTownGround.g, kTownGround.b, 120), 24);
            if (zoom_ < 1.6f) continue; // from up here a village is a patch, not houses
            const u32 count = 6 + t.size * 4;
            for (u32 b = 0; b < count; ++b) {
                const u32 h = cell_hash(static_cast<int>(i) * 977 + static_cast<int>(b), 31337);
                const float angle = unit(h) * 6.2831853f;
                const float dist = std::sqrt(unit(h >> 7)) * radius * 0.86f;
                const Vec2 centre{at.x + std::cos(angle) * dist, at.y + std::sin(angle) * dist};
                const float w = 4.0f + unit(h >> 13) * 3.5f;
                const float d = 3.5f + unit(h >> 19) * 2.5f;
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
                const float head = std::max(0.55f, px(2.5f));
                r.fill_circle(at.x + 0.5f, at.y + 0.7f, head * 1.15f, rgba(20, 22, 20, 90), 10);
                r.fill_circle(at.x, at.y, head * 1.15f, kSignalPost, 10);
                r.fill_circle(at.x, at.y, head * 0.72f, lit, 10);
            }
        }
        r.set_layer(0);
    }

    /// The light "paper" shell: what the player spends, what they are trying
    /// to achieve, and what they can build.
    UiStyle paper_skin() const {
        UiStyle skin;
        skin.panel = rgba(246, 246, 243, 242);
        skin.panel_border = rgba(198, 198, 192, 255);
        skin.title = rgb(38, 40, 46);
        skin.text = rgb(46, 48, 54);
        skin.text_dim = rgb(122, 124, 130);
        skin.button = rgb(233, 233, 229);
        skin.button_hover = rgb(220, 226, 236);
        skin.button_active = kAutobahnBlue;
        skin.button_border = rgba(196, 198, 200, 255);
        skin.accent = kAutobahnBlue;
        skin.track = rgb(214, 214, 208);
        skin.tooltip = rgba(28, 30, 36, 244);
        skin.rounding = 5;
        skin.item_height = 24;
        return skin;
    }

    /// The faint grid the player is snapping to.
    void draw_grid(Renderer2D& r) {
        const Rect view_rect = visible_world();
        if (view_rect.w / kGridStep > 260) return; // too far out to be anything but haze
        StrokeStyle line;
        line.width = std::max(0.08f, px(1));
        set_color(line, rgba(255, 255, 255, 26));
        for (float x = std::floor(view_rect.x / kGridStep) * kGridStep; x < view_rect.right(); x += kGridStep)
            r.draw_polyline({{x, view_rect.y}, {x, view_rect.bottom()}}, line);
        for (float y = std::floor(view_rect.y / kGridStep) * kGridStep; y < view_rect.bottom(); y += kGridStep)
            r.draw_polyline({{view_rect.x, y}, {view_rect.right(), y}}, line);
    }

    void draw_hud(Renderer2D& r) {
        if (!app_) return;
        const UiStyle skin = paper_skin();
        ui_.begin(&r, app_->ui_input(), nullptr, skin);
        char line[96];

        // What it costs, against what the city is trying to reach.
        ui_.begin_panel(kMoneyPanel);
        std::snprintf(line, sizeof line, "%.0f EUR  /  %.0f", money_, kMoneyTarget);
        ui_.label(line);
        ui_.progress(clampf(money_ / kMoneyTarget, 0, 1), kMoneyPanel.w - 16, kAutobahnBlue);
        ui_.end_panel();

        // The goals: which town pairs still have no road between them.
        ui_.begin_panel(kGoalPanel, "Routes");
        const usize open_routes = routes_.size() - routes_connected();
        if (open_routes == 0) std::snprintf(line, sizeof line, "all %zu connected", routes_.size());
        else std::snprintf(line, sizeof line, "%zu of %zu unconnected", open_routes, routes_.size());
        ui_.label(line, open_routes != 0);
        std::snprintf(line, sizeof line, "%zu cars, %u arrived", cars_.count(), arrived_);
        ui_.label(line, true);
        bool overlay = show_flow_;
        if (ui_.toggle("Traffic", overlay)) app_->push_command(kCmdFlow);
        ui_.end_panel();

        // The build panel: the tools, then the road catalogue as cards.
        ui_.begin_panel(kBuildPanel);
        draw_build_tools(r);
        draw_road_cards(r);
        ui_.end_panel();
        ui_.end();

        draw_minimap(r);

        const Camera saved = r.camera();
        r.set_camera(Camera{});
        float radius = 0;
        const float drawn = pending_length(&radius);
        if (drawn > 0.5f) {
            const RoadType* type = road_type_.get(roads_);
            const float price = drawn * (type ? type->cost_per_m : 0);
            const Vec2 tip = shape_ == kShapeFree && !stroke_.empty() ? stroke_.back() : snap_point(mouse_world_);
            const Vec2 at = view(cam_center_, zoom_).to_screen(tip);
            const bool tight = radius > 0 && radius < comfortable_radius(type);
            if (radius > 0) std::snprintf(line, sizeof line, "%.0f m   R %.0f m   %.0f EUR", drawn, radius, price);
            else std::snprintf(line, sizeof line, "%.0f m   %.0f EUR", drawn, price);
            const Color ink = price > money_ ? kBad : tight ? rgb(255, 190, 90) : rgb(255, 240, 200);
            r.draw_text(at.x + 14, at.y + 14, line, ink, 2);
            if (tight) r.draw_text(at.x + 14, at.y + 32, "tight bend", rgb(255, 190, 90), 1);
        }
        if (refusal_[0]) r.draw_text(kBuildPanel.x, kBuildPanel.y - 22, refusal_, kBad, 2);
        r.draw_text(12, kHeight - 210, tool_hint(), rgba(240, 240, 240, 190), 1);
        r.set_camera(saved);
    }

    /// How long the road being drawn is, whichever tool is drawing it.
    float pending_length(float* radius = nullptr) const {
        if (tool_ != kToolBuild) return 0;
        if (shape_ == kShapeFree) {
            if (!drawing_ || stroke_.size() < 2) return 0;
            float drawn = 0;
            for (usize i = 1; i < stroke_.size(); ++i) drawn += distance(stroke_[i], stroke_[i - 1]);
            return drawn;
        }
        const Path ghost = preview_path(radius);
        return ghost.empty() ? 0 : ghost.length();
    }

    const char* tool_hint() const {
        if (tool_ == kToolBuild && shape_ == kShapeStraight)
            return "click where the road starts, then where it ends; right click to stop";
        if (tool_ == kToolBuild && shape_ == kShapeCurve)
            return "click the start, the bend and the end; right click to stop";
        if (tool_ == kToolBuild && shape_ == kShapeSmooth)
            return "start on a road and every click bends away from it without a kink";
        if (tool_ == kToolBuild) return "drag to draw freehand   right click: cancel, drag to pan   wheel: zoom";
        if (tool_ == kToolSignals) return "click a junction to put traffic lights on it, or to take them away";
        if (tool_ == kToolRoundabout) return "click where a roundabout should go; the roads it crosses become its entries";
        return "click a road or junction to remove it   right drag: pan   wheel: zoom";
    }

    void draw_build_tools(Renderer2D& r) {
        const char* names[4] = {"Road", "Roundabout", "Signals", "Remove"};
        const u32 tools[4] = {kToolBuild, kToolRoundabout, kToolSignals, kToolBulldoze};
        for (int i = 0; i < 4; ++i) {
            const Rect box{kToolStrip.x, kToolStrip.y + static_cast<float>(i) * 23, kToolStrip.w, 21};
            if (ui_.button_at(box, names[i])) app_->push_command(kCmdTool, static_cast<i32>(tools[i]));
            outline_if(r, box, tool_ == tools[i]);
        }

        // How the road is drawn, and what the clicks snap to.
        const char* shapes[4] = {"Line", "Curve", "Arc", "Free"};
        const float shape_w = (kShapeStrip.w - 12) / 4;
        for (int i = 0; i < 4; ++i) {
            const Rect box{kShapeStrip.x + static_cast<float>(i) * (shape_w + 4), kShapeStrip.y, shape_w, 21};
            if (ui_.button_at(box, shapes[i])) app_->push_command(kCmdShape, i);
            outline_if(r, box, tool_ == kToolBuild && shape_ == static_cast<u32>(i));
        }
        const Rect angle{kShapeStrip.x, kShapeStrip.y + 25, kShapeStrip.w * 0.5f - 2, 21};
        const Rect grid{kShapeStrip.x + kShapeStrip.w * 0.5f + 2, kShapeStrip.y + 25, kShapeStrip.w * 0.5f - 2, 21};
        if (ui_.button_at(angle, "15 deg")) app_->push_command(kCmdSnapAngle);
        outline_if(r, angle, snap_angle_);
        if (ui_.button_at(grid, "grid")) app_->push_command(kCmdSnapGrid);
        outline_if(r, grid, snap_grid_);
    }

    void outline_if(Renderer2D& r, const Rect& box, bool on) const {
        if (!on) return;
        r.draw_rect({box.x - 2, box.y - 2, box.w + 4, box.h + 4}, kAutobahnBlue);
        r.draw_rect({box.x - 1, box.y - 1, box.w + 2, box.h + 2}, kAutobahnBlue);
    }

    /// One card per road type, each showing a slab of the road it builds,
    /// drawn by the same rules as the real thing, with its price per metre.
    void draw_road_cards(Renderer2D& r) {
        const float top = kBuildPanel.y + 18;
        float x = kShapeStrip.right() + 12;
        for (usize i = 0; i < roads_.size(); ++i) {
            const RoadType& type = roads_.at(i);
            const Rect card{x, top, kCardWidth - 8, kCardHeight + 16};
            if (card.right() > kBuildPanel.right() - 8) break;
            const bool selected = type.id == road_type_.id;
            if (ui_.button_at(card, "")) app_->push_command(kCmdRoad, static_cast<i32>(i));
            draw_road_preview(r, Rect{card.x + 6, card.y + 6, card.w - 12, 40}, type);
            char label[48];
            std::snprintf(label, sizeof label, "%s", type.id.c_str());
            r.draw_text(card.x + 8, card.y + 52, label, rgb(46, 48, 54), 2);
            std::snprintf(label, sizeof label, "%.0f EUR/m", type.cost_per_m);
            r.draw_text(card.x + 8, card.y + 70, label, rgb(122, 124, 130), 1);
            if (selected) {
                r.draw_rect({card.x - 2, card.y - 2, card.w + 4, card.h + 4}, kAutobahnBlue);
                r.draw_rect({card.x - 1, card.y - 1, card.w + 2, card.h + 2}, kAutobahnBlue);
            }
            x += kCardWidth;
        }
    }

    /// A slab of road seen from above, with its own lanes and markings.
    void draw_road_preview(Renderer2D& r, const Rect& box, const RoadType& type) {
        const float lanes = static_cast<float>(std::max(1u, type.lanes_forward + type.lanes_backward));
        const float scale = box.h / (lanes * type.lane_width + 2.0f);
        const float road_h = lanes * type.lane_width * scale;
        const Rect slab{box.x, box.y + (box.h - road_h) * 0.5f, box.w, road_h};
        r.fill_rect(slab, kAsphalt);
        r.fill_rect({slab.x, slab.y - 1.5f, slab.w, 1.5f}, kKerbTop);
        r.fill_rect({slab.x, slab.bottom(), slab.w, 1.5f}, kKerbTop);
        // Lane boundaries: solid between the directions, dashed inside one.
        const float lane_h = road_h / lanes;
        for (u32 i = 1; i < static_cast<u32>(lanes); ++i) {
            const float y = slab.y + lane_h * static_cast<float>(i);
            const bool between = i == type.lanes_backward && type.lanes_forward > 1 && type.lanes_backward > 1;
            if (between) {
                r.fill_rect({slab.x, y - 0.5f, slab.w, 1.2f}, kMarkingWhite);
            } else {
                for (float dx = 2; dx < slab.w - 3; dx += 9) r.fill_rect({slab.x + dx, y - 0.5f, 5, 1.0f}, kMarkingWhite);
            }
        }
    }

    /// The whole network in the corner, with where the camera is looking.
    void draw_minimap(Renderer2D& r) {
        Rect world = network_bounds();
        if (world.w < 1 || world.h < 1) return;
        const Camera saved = r.camera();
        r.set_camera(Camera{});
        r.fill_rect(kMiniMap, rgba(28, 34, 28, 200));
        r.draw_rect(kMiniMap, rgba(210, 210, 205, 180));
        const float scale = std::min((kMiniMap.w - 12) / world.w, (kMiniMap.h - 12) / world.h);
        const Vec2 origin{kMiniMap.x + kMiniMap.w * 0.5f, kMiniMap.y + kMiniMap.h * 0.5f};
        const Vec2 centre{world.x + world.w * 0.5f, world.y + world.h * 0.5f};
        const auto to_map = [&](Vec2 p) { return Vec2{origin.x + (p.x - centre.x) * scale, origin.y + (p.y - centre.y) * scale}; };

        for (const NetEdge& e : net_.edges()) {
            if (!e.alive) continue;
            std::vector<Vec2> line;
            e.path.flatten(8.0f, line);
            for (Vec2& p : line) p = to_map(p);
            StrokeStyle s;
            s.width = std::max(1.0f, e.width * scale);
            set_color(s, show_flow_ ? flow_color(edge_flow(edge_index(e))) : rgba(226, 226, 220, 220));
            if (line.size() >= 2) r.draw_polyline(line, s);
        }
        for (usize i = 0; i < towns_.size(); ++i) {
            const Vec2 at = to_map(towns_.at(i).pos());
            const bool linked = i < access_.size() && access_[i].has;
            r.fill_circle(at.x, at.y, 3, linked ? rgb(120, 200, 130) : rgb(226, 120, 110), 8);
        }
        const Rect view_rect = visible_world();
        const Vec2 a = to_map({view_rect.x, view_rect.y});
        const Vec2 b = to_map({view_rect.right(), view_rect.bottom()});
        r.draw_rect({a.x, a.y, b.x - a.x, b.y - a.y}, rgba(255, 255, 255, 150));
        r.set_camera(saved);
    }

    Color flow_color(float flow) const {
        return flow > 0.6f ? lerp_color(kFlowSlow, kFlowFree, clampf((flow - 0.6f) / 0.4f, 0, 1))
                           : lerp_color(kFlowJam, kFlowSlow, clampf(flow / 0.6f, 0, 1));
    }

    /// Everything worth showing on the minimap.
    Rect network_bounds() const {
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        const auto grow = [&](Vec2 p) {
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        };
        for (usize i = 0; i < towns_.size(); ++i) grow(towns_.at(i).pos());
        for (const NetNode& n : net_.nodes())
            if (n.alive) grow(n.pos);
        if (x1 <= x0 || y1 <= y0) return Rect{};
        const float margin = 120;
        return Rect{x0 - margin, y0 - margin, x1 - x0 + margin * 2, y1 - y0 + margin * 2};
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
    std::vector<Vec2> stroke_;   ///< freehand only
    std::vector<Vec2> points_;   ///< the clicks of a straight or a curve so far
    u32 stroke_from_ = kNone;
    u32 shape_ = kShapeStraight;
    Vec2 entry_dir_{}; ///< the direction a smooth chain leaves along; zero = free
    bool snap_angle_ = true;
    bool snap_grid_ = false;
    bool drawing_ = false;
    Vec2 cam_center_{0, 0};
    Vec2 cam_target_{0, 0};
    float zoom_ = 2.2f;
    float zoom_target_ = 2.2f;
    std::vector<Island> islands_;
    std::vector<u32> pockets_; ///< edges that carry a turn pocket, for the taper marking
    bool auto_pockets_ = true;
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
    Vec2 mouse_world_{};
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
