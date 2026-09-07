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
#include <map>
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
const Color kRoofWork = rgb(104, 112, 122);
const Color kRoofShop = rgb(158, 140, 104);
/// European place-name plates: yellow for a village, blue for a main road
/// destination, green for a trunk road, white for anything local.
const Color kPlate[4] = {rgb(226, 190, 60), rgb(40, 82, 158), rgb(30, 110, 76), rgb(96, 100, 110)};
const Color kAutobahnBlue = rgb(26, 82, 158);
constexpr float kTownReach = 140.0f;   ///< how far an exit looks for a road to join
constexpr float kDoorReach = 40.0f;    ///< how far a building looks for its road
constexpr float kSecondsPerHour = 25.0f; ///< a day is ten minutes at normal speed
constexpr float kGrowEvery = 6.0f;     ///< seconds between rounds of new buildings
constexpr u32 kGrowPerRound = 3;
constexpr float kGrowChance = 0.22f;
constexpr float kLotSpacing = 24.0f;   ///< metres between building plots along a road
constexpr float kSetback = 9.0f;       ///< from the kerb to the building
constexpr usize kMaxBuildings = 360;
constexpr float kReferenceSpeed = 11.0f; ///< m/s, what a trip should average on clear roads
constexpr float kLateFactor = 1.7f;
constexpr float kStartingMoney = 20000;
constexpr usize kUndoDepth = 16;
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
constexpr float kSeamOverlap = 0.6f;     ///< how far a seam zone's paint runs into the road
constexpr float kKerbRadius = 6.0f;      ///< how a junction's corners are rounded
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
    kCmdUndo = 8,
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
const Rect kGoalPanel{kWidth - 420, 10, 408, 120};
const Rect kBuildPanel{kWidth * 0.5f - 448, kHeight - 132, 896, 120};
const Rect kMiniMap{12, kHeight - 184, 172, 172};
const Rect kToolStrip{kBuildPanel.x + 10, kBuildPanel.y + 22, 112, 92};
const Rect kShapeStrip{kBuildPanel.x + 130, kBuildPanel.y + 16, 216, 72};
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

/// A building beside a road. Homes send people to work and to the shops
/// and want them back; workplaces and shops receive them. Buildings appear
/// by themselves along the roads the player builds, and grow when the trips
/// they generate arrive in good time.
struct Building {
    Vec2 pos;
    u32 kind = 0;      ///< 0 home, 1 workplace, 2 shop
    u32 size = 1;      ///< floors, roughly: how many trips it makes and takes
    u32 edge = 0;      ///< the road it stands beside
    float along = 0;   ///< metres along that road
    float side = 1;    ///< which side of the road, +1 or -1
    float pending = 0; ///< trips waiting to set off
    u32 served = 0;
    u32 late = 0;
    u32 lost = 0;      ///< wanted to go somewhere no road reaches

    void reflect(Visitor& v) {
        v.field("pos", pos);
        v.field("kind", kind, {.min = 0, .max = 2});
        v.field("size", size, {.min = 1, .max = 8});
        v.field("edge", edge);
        v.field("along", along);
        v.field("side", side);
        v.field("pending", pending);
        v.field("served", served);
        v.field("late", late);
        v.field("lost", lost);
    }
};

constexpr u32 kHome = 0;
constexpr u32 kWork = 1;
constexpr u32 kShop = 2;

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

/// What an edit could change, saved before it happens so it can be put back.
struct BeforeImage {
    std::vector<u8> bytes; ///< the network, the islands, the signals, the pockets
    float money = 0;

    void reflect(Visitor& v) {
        v.field("bytes", bytes);
        v.field("money", money);
    }
};

/// What the game knows about a car beyond what the agent itself carries.
/// Indexed like the agents, so it survives in the saved state.
struct CarInfo {
    u32 type = 0;
    u32 dest_edge = 0; ///< a TrackRef: edge indices stay valid across rebuilds
    u32 dest_track = 0;
    u32 route = kNone; ///< which town pair this trip serves, for through traffic
    u32 to_building = kNone; ///< the door it is going to, for local traffic
    u64 started = 0;   ///< tick it set off, for the travel time
    float expected = 0; ///< seconds the trip should take with the roads clear

    void reflect(Visitor& v) {
        v.field("type", type);
        v.field("dest_edge", dest_edge);
        v.field("dest_track", dest_track);
        v.field("route", route);
        v.field("to_building", to_building);
        v.field("started", started);
        v.field("expected", expected);
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
            if (cfg.args[i] == "--demo") demo_ = std::stoi(cfg.args[i + 1]);
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
        net_.set_track_trim([this](const CurveNetwork&, u32 node, u32 edge) { return trim_at(node, edge); });
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
        buildings_.clear();
        doors_.clear();
        hour_ = 7.5f;
        grow_timer_ = 0;
        trips_served_ = trips_late_ = trips_lost_ = 0;
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
        history_.clear();
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
        hour_ = std::fmod(hour_ + ctx.dt / kSecondsPerHour, 24.0f);
        grow_timer_ += ctx.dt;
        if (grow_timer_ >= kGrowEvery) {
            grow_timer_ -= kGrowEvery;
            grow_buildings();
        }
        for (u32 i = 0; i < static_cast<u32>(cars_.agents().size()); ++i) {
            if (!cars_.valid(i) || !cars_.agent(i).done) continue;
            arrive(i, ctx.tick);
            cars_.remove(i);
        }
        if (access_version_ != net_.version()) {
            sync_signals();
            refresh_connections();
            refresh_doors();
        }
        step_signals(ctx.dt);
        apply_speed_limits();
        measure_flow(ctx.dt);
        send_trips(ctx);
        send_local_trips(ctx);
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
        v.field("history", history_);
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
        v.field("zoom_anchor", zoom_anchor_);
        v.field("signals", signals_);
        v.field("town_access", access_);
        v.field("routes", routes_);
        v.field("buildings", buildings_);
        v.field("doors", doors_);
        v.field("hour", hour_, {.min = 0, .max = 24});
        v.field("grow_timer", grow_timer_);
        v.field("trips_served", trips_served_, {.read_only = true});
        v.field("trips_late", trips_late_, {.read_only = true});
        v.field("trips_lost", trips_lost_, {.read_only = true});
        v.field("access_version", access_version_);
        v.field("clock", clock_);
        v.field("flow", flow_);
        v.field("money", money_);
        v.field("spent", spent_, {.read_only = true});
        v.field("show_flow", show_flow_);
        v.field("demand", demand_, {.min = 0.1f, .max = 20, .speed = 0.1f});
        v.field("local_demand", local_demand_, {.min = 0.1f, .max = 20, .speed = 0.1f});
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
        draw_buildings(r);
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
    /// The centre the view is drawn from: the smoothed pan, corrected so the
    /// point under the cursor holds still while the zoom catches up with it.
    Vec2 view_center() const {
        const Vec2 off = zoom_anchor_ - Vec2{kWidth * 0.5f, kHeight * 0.5f};
        return cam_center_ + off * (1.0f / zoom_target_ - 1.0f / zoom_);
    }
    Vec2 to_world(const InputFrame& in) const {
        return view(view_center(), zoom_).to_world({static_cast<float>(in.mouse_x), static_cast<float>(in.mouse_y)});
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
        case kCmdUndo: undo(); break;
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
            zoom_anchor_ = mouse;
            const Vec2 before = view(cam_target_, zoom_target_).to_world(mouse);
            zoom_target_ = clampf(zoom_target_ * std::pow(1.32f, static_cast<float>(in.wheel)), 0.35f, 24.0f);
            const Vec2 after = view(cam_target_, zoom_target_).to_world(mouse);
            cam_target_ = cam_target_ + (before - after);
        }
        if (in.down(Button::MouseRight) && prev_input_.down(Button::MouseRight)) {
            cam_target_.x -= static_cast<float>(in.mouse_x - prev_input_.mouse_x) / zoom_target_;
            cam_target_.y -= static_cast<float>(in.mouse_y - prev_input_.mouse_y) / zoom_target_;
        }
        // Plain arithmetic smoothing on the fixed step: the view is part of
        // the simulation, so rewinding gives back the camera motion too.
        const float pan_k = clampf(16.0f * dt, 0, 1);
        const float zoom_k = clampf(28.0f * dt, 0, 1);
        cam_center_ = cam_center_ + (cam_target_ - cam_center_) * pan_k;
        // Zoom in log space, so a step in and a step out feel the same size.
        zoom_ = std::exp(std::log(zoom_) + (std::log(zoom_target_) - std::log(zoom_)) * zoom_k);
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
            if (release && !on_hud && hover_node_ != kNone) {
                remember();
                toggle_signal(hover_node_);
            }
        } else if (tool_ == kToolBulldoze) {
            if (release && !on_hud) {
                Vec2 at{};
                bool removed = false;
                if (hover_node_ != kNone || hover_edge_ != kNone) remember();
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
    /// A line the player can snap to while building, and where it comes from.
    struct Guide {
        Vec2 from;
        Vec2 dir;
        u8 kind; ///< 0 angle step, 1 in line with a road, 2 square to a road
    };

    /// Everything worth snapping to from the point being drawn from: the
    /// directions of the roads at the anchor and their perpendiculars, and
    /// the angle steps in between. In line with a road wins over a plain
    /// angle, because that is what keeps an interchange square.
    std::vector<Guide> guides_from(Vec2 from) const {
        std::vector<Guide> out;
        const u32 node = net_.nearest_node(from, 1.0f);
        if (node != kNone) {
            for (u32 e : net_.edges_at(node)) {
                const NetEdge& edge = net_.edge(e);
                if (edge.path.empty()) continue;
                const Vec2 out_dir = edge.a == node ? edge.path.segments.front().direction(0)
                                                    : edge.path.segments.back().direction(1) * -1.0f;
                out.push_back(Guide{from, out_dir * -1.0f, 1}); // carrying straight on
                out.push_back(Guide{from, perp(out_dir), 2});
                out.push_back(Guide{from, perp(out_dir) * -1.0f, 2});
            }
        }
        if (snap_angle_) {
            const float step = kAngleStep * 3.14159265f / 180.0f;
            for (int i = 0; i < 24; ++i) out.push_back(Guide{from, Vec2{std::cos(i * step), std::sin(i * step)}, 0});
        }
        return out;
    }

    /// The guide the cursor is on, if it is close enough to count.
    const Guide* active_guide(Vec2 from, Vec2 at, const std::vector<Guide>& guides) const {
        const Vec2 delta = at - from;
        const float len = length(delta);
        if (len < 0.5f) return nullptr;
        const float reach = std::max(px(7), 1.5f);
        const Guide* best = nullptr;
        float best_score = 1e30f;
        for (const Guide& g : guides) {
            const float along = dot(delta, g.dir);
            if (along <= 0) continue;
            const float off = std::fabs(cross(g.dir, delta));
            if (off > reach) continue;
            // A road line beats an angle step at the same distance.
            const float score = off + (g.kind == 0 ? reach * 0.5f : 0.0f);
            if (score < best_score) {
                best_score = score;
                best = &g;
            }
        }
        return best;
    }

    /// The radius an interchange designer would pick: a round number.
    static float nice_radius(float radius) {
        const float step = radius < 60 ? 5.0f : radius < 200 ? 10.0f : 25.0f;
        return std::max(step, std::round(radius / step) * step);
    }

    Vec2 snap_point(Vec2 world) const {
        if (hover_node_ != kNone) return net_.node(hover_node_).pos;
        if (hover_edge_ != kNone) {
            const PathSampler s(net_.edge(hover_edge_).path, 0.25f);
            if (!s.empty()) return s.closest(world).position;
        }
        Vec2 at = world;
        if (!points_.empty()) {
            const Vec2 from = points_.back();
            const std::vector<Guide> guides = guides_from(from);
            if (const Guide* g = active_guide(from, at, guides)) at = from + g->dir * dot(at - from, g->dir);
            // In arc mode the end is slid along its own direction so the bend
            // comes out with a radius you could write on a drawing.
            if (shape_ == kShapeSmooth && length_sq(entry_dir_) > 0.1f) {
                float radius = 0;
                traffic::arc_between(from, entry_dir_, at, &radius);
                if (radius > 0) {
                    const Vec2 chord = at - from;
                    const float d = length(chord);
                    const float sin_theta = d > 0 ? std::fabs(cross(normalize(entry_dir_), chord / d)) : 0;
                    if (sin_theta > 0.05f) at = from + chord / d * (2.0f * nice_radius(radius) * sin_theta);
                }
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
        remember();
        auto standing = capture_cars(); // build_road rebuilds, which invalidates every route
        const traffic::BuildResult built = traffic::build_road(net_, path, spec, snap_range());
        if (!built.ok) {
            refusal_ = built.refusal;
            history_.pop_back(); // nothing happened
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
            // Both exits pull: a bigger road beyond the edge sends more, and
            // through traffic has its rush hours like everyone else.
            const float weight = static_cast<float>(towns_.at(r.from).size + towns_.at(r.to).size) * 0.5f;
            const float rhythm = 0.35f + departure_rate(kHome, hour_) * 0.5f;
            r.pending += weight * demand_ * rhythm / 60.0f * ctx.dt;
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
        CarInfo info;
        info.type = type_index;
        info.dest_edge = dest.edge;
        info.dest_track = dest.track;
        info.route = route_index;
        info.started = tick;
        info.expected = route.cost / kReferenceSpeed + 20.0f;
        car_info_[id] = info;
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

    // ---- the city ---------------------------------------------------------------
    //
    // Local traffic comes from buildings that grow along the roads. A home
    // sends people out in the morning and takes them back in the evening;
    // workplaces and shops are where they go. The rhythm is the day.

    /// How keen a building of this kind is to send someone out at this hour,
    /// as a share of its peak.
    static float departure_rate(u32 kind, float hour) {
        const auto bump = [hour](float centre, float width) {
            const float d = std::fabs(hour - centre);
            return std::max(0.0f, 1.0f - d / width);
        };
        switch (kind) {
        case kHome: return 0.08f + bump(8.0f, 1.8f) + 0.5f * bump(18.0f, 2.5f) + 0.25f * bump(12.5f, 2.0f);
        case kWork: return 0.05f + bump(17.0f, 1.8f) + 0.3f * bump(12.5f, 1.5f);
        default: return 0.05f + 0.6f * bump(13.0f, 4.0f) + 0.5f * bump(18.5f, 2.5f);
        }
    }

    /// Where someone leaving this kind of building wants to go at this hour.
    u32 destination_kind(u32 from_kind, float hour) {
        if (from_kind != kHome) return kHome;
        const bool morning = hour > 6 && hour < 10.5f;
        const float roll = rng_.range_f(0, 1);
        if (morning) return roll < 0.8f ? kWork : kShop;
        return roll < 0.35f ? kWork : kShop;
    }

    /// Find each building's door on the network again after the roads changed.
    void refresh_doors() {
        doors_.assign(buildings_.size(), TownAccess{});
        for (usize i = 0; i < buildings_.size(); ++i) {
            Location at{};
            if (!net_.locate(buildings_[i].pos, at, kDoorReach)) continue;
            doors_[i] = TownAccess{true, at.track.edge, at.track.track, at.distance};
        }
    }

    /// Every so often the roads attract new buildings: a stretch of street
    /// with nothing beside it gets something, homes most often.
    void grow_buildings() {
        if (buildings_.size() >= kMaxBuildings) return;
        u32 added = 0;
        for (u32 e = 0; e < static_cast<u32>(net_.edges().size()) && added < kGrowPerRound; ++e) {
            if (!net_.valid_edge(e)) continue;
            const RoadType* type = road_of(e);
            if (!type || type->speed_kmh > 70 || is_ring(e)) continue; // nothing lives on a motorway
            const NetEdge& edge = net_.edge(e);
            const PathSampler along(edge.path, 0.5f);
            const float length = along.length();
            if (length < kLotSpacing * 1.5f) continue;
            for (float d = kLotSpacing * 0.75f; d < length - kLotSpacing * 0.5f && added < kGrowPerRound; d += kLotSpacing) {
                const float side = rng_.range_f(0, 1) < 0.5f ? 1.0f : -1.0f;
                if (rng_.range_f(0, 1) > kGrowChance) continue;
                const PathPoint p = along.at(d);
                const Vec2 at = p.position + perp(p.direction) * (side * (edge.width * 0.5f + kSetback));
                if (crowded(at)) continue;
                Building b;
                b.pos = at;
                const float roll = rng_.range_f(0, 1);
                b.kind = roll < 0.58f ? kHome : roll < 0.86f ? kWork : kShop;
                b.edge = e;
                b.along = d;
                b.side = side;
                buildings_.push_back(b);
                ++added;
            }
        }
        if (added) refresh_doors();
    }

    bool crowded(Vec2 at) const {
        for (const Building& b : buildings_)
            if (distance(b.pos, at) < kLotSpacing * 0.9f) return true;
        for (usize i = 0; i < towns_.size(); ++i)
            if (distance(towns_.at(i).pos(), at) < 60.0f) return true;
        for (const Island& island : islands_)
            if (distance(island.centre, at) < island.radius + kSetback + 6) return true;
        return false;
    }

    /// Buildings send their people out; each trip becomes a car at the door.
    void send_local_trips(const TickContext& ctx) {
        if (vehicles_.size() == 0 || buildings_.empty()) return;
        for (u32 i = 0; i < static_cast<u32>(buildings_.size()); ++i) {
            Building& b = buildings_[i];
            b.pending += departure_rate(b.kind, hour_) * static_cast<float>(b.size) * local_demand_ / 60.0f * ctx.dt;
            b.pending = std::min(b.pending, 3.0f);
            while (b.pending >= 1.0f) {
                if (cars_.count() >= max_cars_) return;
                const int sent = send_from(i, ctx.tick);
                if (sent == 0) break; // no room at the door, try again next tick
                b.pending -= 1.0f;
                if (sent < 0) {
                    ++b.lost;
                    ++trips_lost_;
                }
            }
        }
    }

    /// A trip out of building `from`. 1 sent, 0 no room right now, -1 nowhere to go.
    int send_from(u32 from, u64 tick) {
        if (from >= doors_.size() || !doors_[from].has) return -1;
        const u32 wanted = destination_kind(buildings_[from].kind, hour_);
        const u32 to = pick_building(wanted, from);
        if (to == kNone || !doors_[to].has) return -1;
        const TownAccess& a = doors_[from];
        const TownAccess& d = doors_[to];
        if (!net_.valid_edge(a.edge) || !net_.valid_edge(d.edge)) return -1;
        const Route route = net_.route(Location{TrackRef{a.edge, a.track}, a.distance}, Location{TrackRef{d.edge, d.track}, d.distance});
        if (!route.found || route.steps.empty()) return -1;
        const u32 type_index = pick_type();
        const VehicleType& type = vehicles_.at(type_index);
        const TrackRef first = net_.track_ref(route.steps.front().index);
        if (!room_at(first, a.distance, type.length)) return 0;
        const u32 id = cars_.spawn(route, a.distance, params_of(type), type_index);
        if (id >= car_info_.size()) car_info_.resize(id + 1);
        CarInfo info;
        info.type = type_index;
        info.dest_edge = d.edge;
        info.dest_track = d.track;
        info.to_building = to;
        info.started = tick;
        info.expected = route.cost / kReferenceSpeed + 20.0f;
        car_info_[id] = info;
        ++spawned_;
        return 1;
    }

    /// A building of the wanted kind, bigger ones more often, never itself.
    u32 pick_building(u32 kind, u32 except) {
        float total = 0;
        for (u32 i = 0; i < static_cast<u32>(buildings_.size()); ++i)
            if (i != except && buildings_[i].kind == kind && i < doors_.size() && doors_[i].has)
                total += static_cast<float>(buildings_[i].size);
        if (total <= 0) return kNone;
        float roll = rng_.range_f(0, total);
        for (u32 i = 0; i < static_cast<u32>(buildings_.size()); ++i) {
            if (i == except || buildings_[i].kind != kind || i >= doors_.size() || !doors_[i].has) continue;
            roll -= static_cast<float>(buildings_[i].size);
            if (roll <= 0) return i;
        }
        return kNone;
    }

    void arrive(u32 car, u64 tick) {
        ++arrived_;
        if (car >= car_info_.size()) return;
        const CarInfo& info = car_info_[car];
        if (info.to_building != kNone && info.to_building < buildings_.size()) {
            Building& b = buildings_[info.to_building];
            const float took = static_cast<float>(tick - info.started) / 60.0f;
            const bool late = info.expected > 0 && took > info.expected * kLateFactor;
            ++b.served;
            ++trips_served_;
            if (late) {
                ++b.late;
                ++trips_late_;
                money_ += kFarePerTrip * 0.5f;
            } else {
                money_ += kFarePerTrip;
                // A place that is easy to reach grows.
                if (b.size < 8 && (b.served % 12) == 0) ++b.size;
            }
            return;
        }
        if (info.route >= routes_.size()) return;
        RouteDemand& r = routes_[info.route];
        ++r.arrived;
        ++trips_served_;
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
        remember();
        auto standing = capture_cars();
        const traffic::BuildResult built = traffic::build_roundabout(net_, at, kRoundaboutRadius, spec, snap_range());
        if (!built.ok) {
            refusal_ = built.refusal;
            history_.pop_back();
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
    /// The direction that gains the pocket moves its own lanes outward to
    /// make room, and the pocket takes the slot beside the centre line; the
    /// other direction keeps its offsets, so through traffic on it runs
    /// straight past the junction. The approach swings out over the taper,
    /// which is how a pocket is set out on the ground.
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
            const Track& next = e.tracks[at];
            offset = next.offset - next.width * 0.5f + width * 0.5f;
            for (usize i = at; i < e.tracks.size(); ++i) e.tracks[i].offset += width;
        } else {
            const Track& previous = e.tracks[at - 1];
            offset = previous.offset + previous.width * 0.5f - width * 0.5f;
            for (usize i = 0; i < at; ++i) e.tracks[i].offset -= width;
        }
        Track lane{offset, forward, width, static_cast<u32>(left ? LaneKind::LeftTurn : LaneKind::RightTurn)};
        e.tracks.insert(e.tracks.begin() + static_cast<std::ptrdiff_t>(at), lane);
        e.width += width;
    }

    /// Can traffic arriving on `edge` turn left at `node` at all? At a T the
    /// arm on the far side has nowhere to turn left to, and a pocket there
    /// would only be paint.
    bool has_left_turn(u32 node, u32 edge) const {
        for (u32 c : net_.connectors_at(node)) {
            const Connector& conn = net_.connectors()[c];
            if (conn.from.edge == edge && turn_sense(net_, conn.from, conn.to) < 0) return true;
        }
        return false;
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
            if (arms.size() < 3) continue;
            for (u32 edge : arms)
                if (has_left_turn(node, edge)) changed |= add_pocket(node, edge, true);
        }
        if (changed) net_.rebuild();
    }

    bool is_ring(u32 edge) const {
        const RoadType* type = road_of(edge);
        return type && type->id == "roundabout";
    }

    // ---- undo ------------------------------------------------------------------

    /// Everything an edit can touch, as bytes.
    struct Editable {
        CurveNetwork* net;
        std::vector<Island>* islands;
        std::vector<Signal>* signals;
        std::vector<u32>* pockets;
        void reflect(Visitor& v) {
            v.field("network", *net);
            v.field("islands", *islands);
            v.field("signals", *signals);
            v.field("pockets", *pockets);
        }
    };

    void remember() {
        BeforeImage image;
        Writer w;
        {
            SaveVisitor save(w);
            Editable e{&net_, &islands_, &signals_, &pockets_};
            e.reflect(save);
        }
        image.bytes = w.bytes();
        image.money = money_;
        history_.push_back(std::move(image));
        if (history_.size() > kUndoDepth) history_.erase(history_.begin());
    }

    void undo() {
        if (history_.empty()) return;
        auto standing = capture_cars();
        BeforeImage image = std::move(history_.back());
        history_.pop_back();
        {
            Reader r(image.bytes);
            LoadVisitor load(r);
            Editable e{&net_, &islands_, &signals_, &pockets_};
            e.reflect(load);
        }
        money_ = image.money;
        ++signal_version_;
        net_.rebuild();
        restore_cars(standing);
        refresh_connections();
        points_.clear();
        entry_dir_ = Vec2{};
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
        if (demo_ == 1) build_demo_roads();
        if (demo_ == 2) build_demo_t();
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

    /// A T junction: an avenue running east to west, a street ending on it.
    void build_demo_t() {
        const auto spec_of = [this](const RoadType* t) { return t ? spec_for(*t) : traffic::RoadSpec{}; };
        traffic::build_road(net_, Path::from_polyline({{-400, 0}, {400, 0}}), spec_of(roads_.find("avenue")), 10.0f);
        traffic::build_road(net_, Path::from_polyline({{0, 400}, {0, 0}}), spec_of(roads_.find("street")), 10.0f);
        std::vector<u32> nodes;
        for (u32 n = 0; n < static_cast<u32>(net_.nodes().size()); ++n)
            if (net_.valid_node(n)) nodes.push_back(n);
        if (auto_pockets_) ensure_pockets(nodes);
        net_.rebuild();
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
        const Camera c = view(view_center(), zoom_);
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
        seam_shift_.assign(net_.nodes().size(), 0);
        for (usize i = 0; i < net_.nodes().size(); ++i)
            if (net_.node(static_cast<u32>(i)).alive && seam_shifts(static_cast<u32>(i))) seam_shift_[i] = 1;
        for (const NetEdge& e : net_.edges()) {
            if (!e.alive) continue;
            build_edge_surface(e);
            build_edge_markings(e);
        }
        for (usize i = 0; i < net_.nodes().size(); ++i) {
            if (!net_.node(static_cast<u32>(i)).alive) continue;
            build_junction(static_cast<u32>(i));
            if (seam_shift_[i]) build_seam_zone(static_cast<u32>(i));
        }
        build_junction_guides();
    }

    /// Where the paving of an edge lies across its centreline. Lanes need not
    /// be symmetric (a turn pocket, a bus lane one way), so the surface
    /// follows the lanes rather than half the width each side.
    static void section_extent(const NetEdge& e, float& left, float& right) {
        left = 1e30f;
        right = -1e30f;
        for (const Track& t : e.tracks) {
            left = std::min(left, t.offset - t.width * 0.5f);
            right = std::max(right, t.offset + t.width * 0.5f);
        }
        if (e.tracks.empty()) {
            left = -e.width * 0.5f;
            right = e.width * 0.5f;
        }
    }

    void build_edge_surface(const NetEdge& e) {
        float left = 0, right = 0;
        section_extent(e, left, right);
        // A shifting seam paves itself along its lanes; the straight body
        // would poke out past the kerb that swings away from it.
        const float total = e.path.length();
        const float head = seam_shift(e.a) ? net_.node(e.a).radius : 0.0f;
        const float tail = seam_shift(e.b) ? net_.node(e.b).radius : 0.0f;
        if (total - head - tail < 0.5f) return;
        const Path body = (head > 0 || tail > 0) ? e.path.sub_path(head, total - tail) : e.path;
        const std::vector<Vec2> line = body.offset((left + right) * 0.5f, 0.35f);
        if (line.size() < 2) return;
        StrokeStyle s;
        s.width = right - left;
        s.join = LineJoin::Miter;
        s.cap = LineCap::Butt;
        set_color(s, kAsphalt);
        build_polyline(line, s, surface_mesh_);
    }

    /// One road arriving at a junction, seen from the node.
    struct Arm {
        u32 edge;
        float angle;
        Vec2 dir;  ///< away from the node
        float lo;  ///< kerb offsets along perp(dir): the two edges of the paving
        float hi;
        float cut; ///< how far out along the arm the junction's paving reaches
        Vec2 at() const { return origin + dir * cut; }
        Vec2 origin;
    };

    /// The arms of a junction in ring order, with the kerb offsets of each
    /// measured the way the arm leaves the node.
    std::vector<Arm> arms_of(u32 node) const {
        std::vector<Arm> arms;
        const NetNode& n = net_.node(node);
        for (u32 index : net_.edges_at(node)) {
            const NetEdge& e = net_.edge(index);
            if (e.path.empty()) continue;
            Vec2 dir;
            if (e.a == node) dir = e.path.segments.front().direction(0);
            else if (e.b == node) dir = e.path.segments.back().direction(1) * -1.0f;
            else continue;
            if (length_sq(dir) < 1e-6f) continue;
            // Offsets are measured along the edge's own direction; an arm that
            // leaves the node against it sees them mirrored.
            float left = 0, right = 0;
            section_extent(e, left, right);
            const bool along = e.a == node;
            arms.push_back(Arm{index, angle_of(dir), dir, along ? left : -right, along ? right : -left, 0, n.pos});
        }
        std::sort(arms.begin(), arms.end(), [](const Arm& a, const Arm& b) { return a.angle < b.angle; });
        return arms;
    }

    /// Where the kerb on the high side of `a` meets the kerb on the low side
    /// of `b`, the next arm round. False when they run parallel.
    static bool kerb_corner(const Arm& a, const Arm& b, Vec2& corner) {
        return line_intersection(a.origin + perp(a.dir) * a.hi, a.dir, b.origin + perp(b.dir) * b.lo, b.dir, corner);
    }

    /// How far the paving reaches along each arm: at least the node's own
    /// radius, and past every corner by enough to round it. The markings on
    /// that arm stop at the same distance, so paint never lands on the pad.
    void cut_arms(std::vector<Arm>& arms, float base) const {
        for (Arm& a : arms) a.cut = base;
        if (arms.size() < 3) return;
        for (usize i = 0; i < arms.size(); ++i) {
            Arm& a = arms[i];
            Arm& b = arms[(i + 1) % arms.size()];
            Vec2 corner;
            if (!kerb_corner(a, b, corner)) continue;
            const float along_a = dot(corner - a.origin, a.dir);
            const float along_b = dot(corner - b.origin, b.dir);
            const float room = kKerbRadius * 0.9f;
            if (along_a > -50 && along_a < 80) a.cut = std::max(a.cut, along_a + room);
            if (along_b > -50 && along_b < 80) b.cut = std::max(b.cut, along_b + room);
        }
    }

    float pad_base(u32 node) const {
        const std::vector<u32> edges = net_.edges_at(node);
        const bool seam = edges.size() == 2;
        float radius = seam ? 0.0f : net_.node(node).radius;
        for (u32 e : edges) radius = std::max(radius, net_.edge(e).width * (seam ? 0.5f : 0.55f));
        return radius;
    }

    /// How much of an edge the junction at `node` takes for its own paving.
    float arm_cut(u32 node, u32 edge) const {
        if (!net_.valid_node(node)) return 0;
        std::vector<Arm> arms = arms_of(node);
        if (arms.size() < 3) return 0.0f; // a seam in one road: the markings run through
        cut_arms(arms, pad_base(node));
        for (const Arm& a : arms)
            if (a.edge == edge) return a.cut;
        return pad_base(node);
    }

    /// Where the tracks of an edge stop at a node: at the paving of a
    /// junction, or the node's own radius across a seam, which is the room
    /// the lanes have to shift sideways.
    float trim_at(u32 node, u32 edge) const {
        if (!net_.valid_node(node)) return 0;
        if (net_.edges_at(node).size() >= 3) return arm_cut(node, edge);
        return net_.node(node).radius;
    }

    /// A seam where the lanes on the two sides do not line up: a lane opens
    /// or closes, or the road swings sideways to make room for one.
    bool seam_shifts(u32 node) const {
        const std::vector<u32> edges = net_.edges_at(node);
        if (edges.size() != 2) return false;
        if (net_.edge(edges[0]).tracks.size() != net_.edge(edges[1]).tracks.size()) return true;
        for (u32 c : net_.connectors_at(node)) {
            const Connector& k = net_.connectors()[c];
            if (k.path.empty()) continue;
            const Vec2 d = k.path.segments.front().direction(0);
            if (absf(cross(d, k.path.end() - k.path.start())) > 0.15f) return true;
        }
        return false;
    }
    bool seam_shift(u32 node) const { return node < seam_shift_.size() && seam_shift_[node] != 0; }

    /// The paved area where roads meet: each arm is cut off a little way from
    /// the node and the gaps between them are closed at the point where their
    /// kerbs would cross, rounded off the way a real corner is.
    void build_junction(u32 node) {
        std::vector<Arm> arms = arms_of(node);
        if (arms.size() < 2) return;
        const bool seam = arms.size() == 2;
        if (seam && seam_shift(node)) return; // paves itself along its lanes
        cut_arms(arms, pad_base(node));

        std::vector<Vec2> pad;
        pad.reserve(arms.size() * 9);
        for (usize i = 0; i < arms.size(); ++i) {
            const Arm& a = arms[i];
            const Arm& next = arms[(i + 1) % arms.size()];
            const Vec2 a_lo = a.at() + perp(a.dir) * a.lo;
            const Vec2 a_hi = a.at() + perp(a.dir) * a.hi;
            const Vec2 n_lo = next.at() + perp(next.dir) * next.lo;
            pad.push_back(a_lo);
            pad.push_back(a_hi);
            Vec2 corner;
            if (!kerb_corner(a, next, corner)) continue;
            const float reach = pad_base(node) + std::max(a.hi - a.lo, next.hi - next.lo) * 3.0f + kKerbRadius;
            if (distance(corner, a.origin) > reach) continue;
            if (seam) {
                pad.push_back(corner);
                continue;
            }
            // The corner, rounded: a quadratic through the corner point from
            // one kerb to the other is within a few centimetres of the arc.
            const float back = std::min({kKerbRadius, distance(corner, a_hi) * 0.7f, distance(corner, n_lo) * 0.7f});
            const Vec2 from = corner + normalize(a_hi - corner) * back;
            const Vec2 to = corner + normalize(n_lo - corner) * back;
            for (int k = 0; k <= 6; ++k) {
                const float t = static_cast<float>(k) / 6;
                const float u = 1 - t;
                pad.push_back(from * (u * u) + corner * (2 * u * t) + to * (t * t));
            }
        }
        build_polygon(pad, kAsphalt.r / 255.0f, kAsphalt.g / 255.0f, kAsphalt.b / 255.0f, 1.0f, surface_mesh_);
    }

    /// Across a shifting seam every lane follows its connector: the lane
    /// lines swing with the road, and a pocket opens in the wedge beside the
    /// lane that moved over, which is how it is set out on the ground.
    void build_seam_zone(u32 node) {
        struct SeamLane {
            Path path;     ///< across the seam, in the lane's own travel direction
            float lateral; ///< where it starts, across the seam in one fixed frame
            float sign;    ///< +1 when the lane's own right is the frame's positive side
            float width;
            LaneKind kind;
            bool with_axis;
        };
        const std::vector<Arm> arms = arms_of(node);
        if (arms.size() != 2) return;
        const Vec2 axis = arms[0].dir * -1.0f;
        const Vec2 across = perp(axis);
        const Vec2 origin = net_.node(node).pos;
        std::vector<SeamLane> lanes;
        const auto add_lane = [&](const Path& path, const Track& t) {
            if (path.empty()) return;
            const Vec2 d = path.segments.front().direction(0);
            lanes.push_back(SeamLane{path, dot(path.start() - origin, across), dot(perp(d), across) >= 0 ? 1.0f : -1.0f,
                                     t.width, kind_of(t), dot(d, axis) > 0});
        };
        for (u32 c : net_.connectors_at(node)) {
            const Connector& k = net_.connectors()[c];
            add_lane(k.path, net_.edge(k.from.edge).tracks[k.from.track]);
        }
        // Lanes that are not for cars have no connectors, but a cycle lane or
        // a shoulder does carry on across the seam: each pairs with the lane
        // of its kind the same distance from the kerb on the other side.
        const auto kind_rank = [](const NetEdge& e, u32 track) {
            const Track& lane = e.tracks[track];
            const float side = traffic::kerb_side(lane.forward);
            u32 rank = 0;
            for (const Track& t : e.tracks)
                if (t.forward == lane.forward && kind_of(t) == kind_of(lane) && (t.offset - lane.offset) * side > 0.01f) ++rank;
            return rank;
        };
        for (int pass = 0; pass < 2; ++pass) {
            const u32 from_edge = arms[pass].edge, to_edge = arms[1 - pass].edge;
            const NetEdge& in = net_.edge(from_edge);
            const NetEdge& out = net_.edge(to_edge);
            for (u32 i = 0; i < static_cast<u32>(in.tracks.size()); ++i) {
                const Track& t = in.tracks[i];
                if (is_carriageway(kind_of(t))) continue;
                if ((t.forward ? in.b : in.a) != node) continue; // has to arrive here
                for (u32 j = 0; j < static_cast<u32>(out.tracks.size()); ++j) {
                    const Track& u = out.tracks[j];
                    if (kind_of(u) != kind_of(t) || (u.forward ? out.a : out.b) != node) continue;
                    if (kind_rank(in, i) != kind_rank(out, j)) continue;
                    const Path& a = net_.track_path(TrackRef{from_edge, i});
                    const Path& b = net_.track_path(TrackRef{to_edge, j});
                    if (a.empty() || b.empty()) continue;
                    const Vec2 p0 = a.end(), p3 = b.start();
                    const Vec2 d0 = a.segments.back().direction(1), d3 = b.segments.front().direction(0);
                    const float reach = std::max(distance(p0, p3) * 0.4f, 0.01f);
                    Path path;
                    path.segments.push_back({p0, p0 + d0 * reach, p3 - d3 * reach, p3});
                    add_lane(path, t);
                    break;
                }
            }
        }
        if (lanes.empty()) return;
        std::sort(lanes.begin(), lanes.end(), [](const SeamLane& a, const SeamLane& b) { return a.lateral < b.lateral; });

        // One edge of a lane: `side` is +1 on the frame's positive side, and
        // `extra` pushes the line further out. Always runs with the axis.
        const auto boundary = [&](const SeamLane& l, float side, float extra) {
            std::vector<Vec2> line = l.path.offset(l.sign * side * (l.width * 0.5f + extra), 0.3f);
            if (!l.with_axis) std::reverse(line.begin(), line.end());
            // A little way into the road on either side, so nothing shows
            // through the join with the body's own paint.
            if (line.size() >= 2) {
                const Vec2 head = normalize(line[0] - line[1]);
                const Vec2 tail = normalize(line.back() - line[line.size() - 2]);
                line.insert(line.begin(), line[0] + head * kSeamOverlap);
                line.push_back(line.back() + tail * kSeamOverlap);
            }
            return line;
        };

        // The paving between the outermost lanes.
        std::vector<Vec2> pad = boundary(lanes.front(), -1.0f, 0.0f);
        const std::vector<Vec2> far = boundary(lanes.back(), 1.0f, 0.0f);
        pad.insert(pad.end(), far.rbegin(), far.rend());
        if (pad.size() >= 3) build_polygon(pad, kAsphalt.r / 255.0f, kAsphalt.g / 255.0f, kAsphalt.b / 255.0f, 1.0f, surface_mesh_);

        usize with = 0, against = 0;
        for (const SeamLane& l : lanes) (l.with_axis ? with : against)++;
        const usize per_side = std::max(with, against);

        // Lanes that are not for cars keep their own surface across the seam.
        for (const SeamLane& l : lanes) {
            if (l.kind == LaneKind::Driving || l.kind == LaneKind::LeftTurn || l.kind == LaneKind::RightTurn) continue;
            StrokeStyle strip;
            strip.width = l.width - 0.1f;
            set_color(strip, l.kind == LaneKind::Cycle    ? kCycleSurface
                             : l.kind == LaneKind::Bus    ? kBusSurface
                             : l.kind == LaneKind::Parking ? kParkingSurface
                                                           : kShoulderSurface);
            add_line(boundary(l, 0.0f, 0.0f), strip);
        }

        StrokeStyle kerb;
        kerb.width = kKerbBand;
        set_color(kerb, kKerbTop);
        StrokeStyle shadow;
        shadow.width = kKerbShadowBand;
        set_color(shadow, kKerbShadow);
        StrokeStyle solid;
        solid.width = marking_width(kNarrowLine);
        set_color(solid, kMarkingWhite);
        for (int end = 0; end < 2; ++end) {
            const SeamLane& l = end == 0 ? lanes.front() : lanes.back();
            const float side = end == 0 ? -1.0f : 1.0f;
            add_line(boundary(l, side, kKerbBand * 0.5f), kerb);
            add_line(boundary(l, side, kKerbBand + kKerbShadowBand * 0.5f), shadow);
            add_line(boundary(l, side, -kEdgeLineInset), solid);
        }

        StrokeStyle lane_line = solid;
        lane_line.dash = dash_mark(per_side);
        lane_line.gap = dash_gap(per_side);
        for (usize i = 0; i + 1 < lanes.size(); ++i) {
            const SeamLane& a = lanes[i];
            const SeamLane& b = lanes[i + 1];
            const std::vector<Vec2> ab = boundary(a, 1.0f, 0.0f);
            const std::vector<Vec2> ba = boundary(b, -1.0f, 0.0f);
            if (a.with_axis != b.with_axis) {
                // Between the directions: solid on both sides, so whatever
                // opens in the wedge between them is bounded. A wide road
                // has its two lines here, the same as along the body.
                const float inset = per_side > 1 ? -kNarrowLine : 0.0f;
                add_line(boundary(a, 1.0f, inset), solid);
                add_line(boundary(b, -1.0f, inset), solid);
                continue;
            }
            add_line(ab, lane_line);
            // The same direction, but the lanes part company: something opens
            // or closes between them, and its edge is drawn solid.
            if (ab.size() >= 2 && ba.size() >= 2 &&
                (distance(ab.front(), ba.front()) > 0.3f || distance(ab.back(), ba.back()) > 0.3f))
                add_line(ba, solid);
        }
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
        float left = 0, right = 0;
        section_extent(e, left, right);

        std::vector<float> forward, backward;
        for (const Track& t : e.tracks) (t.forward ? forward : backward).push_back(t.offset);
        std::sort(forward.begin(), forward.end());
        std::sort(backward.begin(), backward.end());
        const usize per_side = std::max(forward.size(), backward.size());

        // Kerbs: from above a kerb is a pale band with its own shadow outside.
        StrokeStyle kerb;
        kerb.width = kKerbBand;
        set_color(kerb, kKerbTop);
        add_line(body.offset(right + kKerbBand * 0.5f), kerb);
        add_line(body.offset(left - kKerbBand * 0.5f), kerb);
        StrokeStyle shadow;
        shadow.width = kKerbShadowBand;
        set_color(shadow, kKerbShadow);
        add_line(body.offset(right + kKerbBand + kKerbShadowBand * 0.5f), shadow);
        add_line(body.offset(left - kKerbBand - kKerbShadowBand * 0.5f), shadow);

        // Edge lines, set in from the carriageway edge.
        StrokeStyle edge_line;
        edge_line.width = marking_width(kNarrowLine);
        set_color(edge_line, kMarkingWhite);
        add_line(body.offset(right - kEdgeLineInset), edge_line);
        add_line(body.offset(left + kEdgeLineInset), edge_line);

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
        float left = 0, right = 0;
        section_extent(e, left, right);
        if (zebra_at > kZebraDepth && zebra_at < length - kZebraDepth) add_zebra(along, zebra_at, left, right);

        // One arrow per lane, showing where that lane may go. The turns come
        // from the junction's own connectors, so an arrow cannot lie.
        const float arrow_at = stop_at - sign * (kArrowSetback + kArrowLength);
        if (zoom_ >= 5.0f && arrow_at > kArrowLength && arrow_at < length - kArrowLength) {
            for (u32 t = 0; t < static_cast<u32>(e.tracks.size()); ++t) {
                const Track& track = e.tracks[t];
                if (track.forward != at_end) continue; // only the lanes running into this junction
                if (!is_carriageway(kind_of(track))) continue;
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
        // Offsets are measured along the edge, not along the lane's travel.
        const Vec2 base = p.position + perp(p.direction) * offset - forward * (kArrowLength * 0.5f);
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
    void add_zebra(const PathSampler& along, float at, float left, float right) {
        const float lo = left + kZebraInset, hi = right - kZebraInset;
        for (float x = lo; x + kZebraBar <= hi; x += kZebraBar + kZebraGap) {
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
        const usize arms = net_.edges_at(node).size();
        if (arms >= 3) return arm_cut(node, edge_index(e));
        if (arms == 2 && seam_shift(node)) return net_.node(node).radius;
        return 0;
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
            if (!traffic::lane_is_fed(net_, ref) && !seam_shift(lane.forward ? e.a : e.b))
                add_taper(e, along, lane, lane.forward ? 0.0f : length, true);
            if (!traffic::lane_continues(net_, ref) && !seam_shift(lane.forward ? e.b : e.a))
                add_taper(e, along, lane, lane.forward ? length : 0.0f, false);
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

    /// The dotted lines that carry lanes across a junction. Lanes making the
    /// same movement from one road onto another are one group: the group's
    /// outer edges and the boundaries between its lanes are drawn dotted, so
    /// two lanes turning together stay two bounded lanes all the way round.
    void build_junction_guides() {
        if (zoom_ < 5.0f) return;
        StrokeStyle guide;
        guide.width = marking_width(kNarrowLine);
        guide.dash = kGuideDash;
        guide.gap = kGuideGap;
        set_color(guide, kGuideWhite);

        struct Movement {
            u32 connector;
            u32 rank;    ///< of the lane it leaves, from the kerb, so the group sorts kerb to centre
            u32 to_rank; ///< of the lane it arrives in
        };
        struct Key {
            u32 from_edge, to_edge;
            int sense;
            bool operator<(const Key& o) const {
                if (from_edge != o.from_edge) return from_edge < o.from_edge;
                if (to_edge != o.to_edge) return to_edge < o.to_edge;
                return sense < o.sense;
            }
        };
        std::map<Key, std::vector<Movement>> groups;
        for (u32 i = 0; i < static_cast<u32>(net_.connectors().size()); ++i) {
            const Connector& c = net_.connectors()[i];
            if (c.path.empty() || c.length < 3 || net_.edges_at(c.node).size() < 3) continue;
            const int sense = turn_sense(net_, c.from, c.to);
            groups[Key{c.from.edge, c.to.edge, sense}].push_back(
                Movement{i, traffic::rank_from_kerb(net_.edge(c.from.edge), c.from.track),
                         traffic::rank_from_kerb(net_.edge(c.to.edge), c.to.track)});
        }

        for (auto& [key, movements] : groups) {
            // Going straight on, the lane lines stop at the stop line; only the
            // turns are guided. A lone left turn gets the one dotted line on
            // its inner side; two lanes turning together get their edges and
            // the boundary between them, so they stay two lanes all the way.
            if (key.sense == 0) continue;
            // One lane may be allowed into several exit lanes; that is still one
            // lane, guided along its natural exit: the inner lane for a left
            // turn, the kerb lane for a right.
            std::sort(movements.begin(), movements.end(), [&](const Movement& a, const Movement& b) {
                if (a.rank != b.rank) return a.rank < b.rank;
                return key.sense < 0 ? a.to_rank > b.to_rank : a.to_rank < b.to_rank;
            });
            movements.erase(std::unique(movements.begin(), movements.end(),
                                        [](const Movement& a, const Movement& b) { return a.rank == b.rank; }),
                            movements.end());
            const auto lane_width_of = [&](const Movement& m) {
                const Connector& c = net_.connectors()[m.connector];
                return net_.edge(c.from.edge).tracks[c.from.track].width;
            };
            if (movements.size() == 1) {
                if (key.sense > 0) continue; // a lone right turn hugs the kerb and needs no guide
                const Connector& only = net_.connectors()[movements.front().connector];
                add_line(only.path.offset(-kerb_sign(only) * lane_width_of(movements.front()) * 0.5f, 0.3f), guide);
                continue;
            }
            std::sort(movements.begin(), movements.end(), [](const Movement& a, const Movement& b) { return a.rank < b.rank; });
            // Outer edges of the whole movement: the kerb side of the kerb
            // lane and the centre side of the centre lane.
            {
                const Connector& kerb = net_.connectors()[movements.front().connector];
                const Connector& inner = net_.connectors()[movements.back().connector];
                const float side = kerb_sign(kerb);
                add_line(kerb.path.offset(side * lane_width_of(movements.front()) * 0.5f, 0.3f), guide);
                add_line(inner.path.offset(-side * lane_width_of(movements.back()) * 0.5f, 0.3f), guide);
            }
            // Boundaries between neighbouring lanes of the group.
            for (usize i = 1; i < movements.size(); ++i) {
                const Connector& a = net_.connectors()[movements[i - 1].connector];
                const Connector& b = net_.connectors()[movements[i].connector];
                add_line(midline(a.path, b.path), guide);
            }
        }
    }

    /// Which side of a connector's path its kerb is on: +1 for the perp() side.
    /// Forward lanes keep the kerb on the positive offset side, and the
    /// connector runs the same way as the lane it leaves.
    float kerb_sign(const Connector& c) const {
        const Track& lane = net_.edge(c.from.edge).tracks[c.from.track];
        return lane.forward ? 1.0f : -1.0f;
    }

    /// The curve halfway between two paths, sampled by how far along each one is.
    static std::vector<Vec2> midline(const Path& a, const Path& b) {
        const PathSampler sa(a, 0.3f), sb(b, 0.3f);
        std::vector<Vec2> out;
        if (sa.empty() || sb.empty()) return out;
        const int steps = 14;
        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / steps;
            const Vec2 pa = sa.at(t * sa.length()).position;
            const Vec2 pb = sb.at(t * sb.length()).position;
            out.push_back((pa + pb) * 0.5f);
        }
        return out;
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
            draw_guides(r, at);
            r.fill_circle(at.x, at.y, std::max(0.6f, px(3)), rgba(255, 255, 255, 180), 10);
            if (snap_grid_) draw_grid(r);
            // What a click would land on glows, so there is no guessing.
            if (hover_node_ != kNone) {
                const NetNode& n = net_.node(hover_node_);
                r.fill_circle(n.pos.x, n.pos.y, std::max(n.radius, px(6)), rgba(255, 214, 90, 90), 16);
            } else if (hover_edge_ != kNone) {
                StrokeStyle s;
                s.width = std::max(0.25f, px(1.5f));
                set_color(s, rgba(255, 214, 90, 160));
                float left = 0, right = 0;
                section_extent(net_.edge(hover_edge_), left, right);
                r.draw_polyline(net_.edge(hover_edge_).path.offset(right), s);
                r.draw_polyline(net_.edge(hover_edge_).path.offset(left), s);
            }
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
    /// Homes, workplaces and shops: a block with a roof and a shadow, coloured
    /// by what it is, taller ones bigger.
    void draw_buildings(Renderer2D& r) {
        if (zoom_ < 1.2f) return;
        r.set_layer(-1);
        for (u32 i = 0; i < static_cast<u32>(buildings_.size()); ++i) {
            const Building& b = buildings_[i];
            if (!net_.valid_edge(b.edge)) continue;
            const NetEdge& e = net_.edge(b.edge);
            const PathSampler along(e.path, 0.5f);
            if (along.empty()) continue;
            const PathPoint p = along.at(clampf(b.along, 0, along.length()));
            const Vec2 face = p.direction;                  // the building fronts the road
            const Vec2 depth = perp(face) * b.side;          // and stretches away from it
            const float w = 8.0f + static_cast<float>(b.size) * 1.2f;
            const float d = 7.0f + static_cast<float>(b.size) * 1.6f;
            const Vec2 c = b.pos + depth * (d * 0.5f - 2.0f);
            const Vec2 hw = face * (w * 0.5f), hd = depth * (d * 0.5f);
            const Color roof = b.kind == kHome ? lerp_color(kRoofDark, kRoofLight, unit(cell_hash(static_cast<int>(i), 77)))
                               : b.kind == kWork ? kRoofWork
                                                 : kRoofShop;
            const Vec2 shade{1.4f + static_cast<float>(b.size) * 0.3f, 1.8f + static_cast<float>(b.size) * 0.4f};
            r.fill_polygon({c - hw - hd + shade, c + hw - hd + shade, c + hw + hd + shade, c - hw + hd + shade}, kBuildingShadow);
            r.fill_polygon({c - hw - hd, c + hw - hd, c + hw + hd, c - hw + hd}, roof);
            r.fill_polygon({c - hw - hd * 0.12f, c + hw - hd * 0.12f, c + hw + hd * 0.12f, c - hw + hd * 0.12f},
                           lerp_color(roof, rgb(255, 245, 225), 0.22f));
            if (b.kind == kShop && zoom_ > 4) r.fill_rect({c.x - 1.5f, c.y - 1.5f, 3, 3}, rgb(230, 190, 60));
        }
        r.set_layer(0);
    }

    /// An exit: the road carries on off the map, and a sign says where to.
    void draw_towns(Renderer2D& r) {
        r.set_layer(-3);
        for (usize i = 0; i < towns_.size(); ++i) {
            const Town& t = towns_.at(i);
            const Vec2 at = t.pos();
            // The road runs on beyond the edge of the map: a strip that fades out.
            Vec2 outward = normalize(at - Vec2{0, 0});
            if (length_sq(outward) < 0.5f) outward = {1, 0};
            const float width = 6.0f;
            for (int step = 0; step < 6; ++step) {
                const Vec2 a = at + outward * (26.0f * static_cast<float>(step) - 10.0f);
                const Vec2 b = at + outward * (26.0f * static_cast<float>(step + 1) - 10.0f);
                StrokeStyle strip;
                strip.width = width;
                set_color(strip, rgba(kAsphalt.r, kAsphalt.g, kAsphalt.b, static_cast<u8>(200 - step * 32)));
                r.draw_polyline({a, b}, strip);
            }
        }
        r.set_layer(4);
        for (usize i = 0; i < towns_.size(); ++i) {
            const Town& t = towns_.at(i);
            const Vec2 screen = view(view_center(), zoom_).to_screen(t.pos());
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

    /// The rays the cursor can snap to, the one it is on drawn brighter, and
    /// in arc mode the centre of the bend with its radius.
    void draw_guides(Renderer2D& r, Vec2 at) {
        if (points_.empty()) return;
        const Vec2 from = points_.back();
        const std::vector<Guide> guides = guides_from(from);
        const Guide* active = active_guide(from, mouse_world_, guides);
        // Long enough to leave the screen, never so long that the dashes run
        // into float trouble far from the anchor.
        const float reach = std::min(std::max(visible_world().w, visible_world().h), 1500.0f);
        for (const Guide& g : guides) {
            if (g.kind == 0 && &g != active) continue; // angle steps only show when caught
            StrokeStyle ray;
            ray.width = std::max(0.08f, px(1));
            ray.dash = px(6);
            ray.gap = px(4);
            const bool on = &g == active;
            set_color(ray, on ? rgba(255, 230, 140, 220) : rgba(255, 255, 255, 60));
            r.draw_polyline({from, from + g.dir * reach}, ray);
        }
        if (shape_ == kShapeSmooth && length_sq(entry_dir_) > 0.1f) {
            float radius = 0;
            const Path arc = traffic::arc_between(from, entry_dir_, at, &radius);
            if (radius > 0 && !arc.empty()) {
                const Vec2 side = perp(normalize(entry_dir_));
                const float sign = cross(normalize(entry_dir_), at - from) >= 0 ? 1.0f : -1.0f;
                const Vec2 centre = from + side * (radius * sign);
                StrokeStyle spoke;
                spoke.width = std::max(0.08f, px(1));
                set_color(spoke, rgba(255, 255, 255, 90));
                const PathSampler sampler(arc, 0.3f);
                const Vec2 mid = sampler.at(sampler.length() * 0.5f).position;
                r.draw_polyline({centre, mid}, spoke);
                r.fill_circle(centre.x, centre.y, std::max(0.4f, px(2.5f)), rgba(255, 255, 255, 140), 8);
            }
        }
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

        // The city: the time, what has grown, and how the trips are going.
        std::snprintf(line, sizeof line, "%02d:%02d", static_cast<int>(hour_), static_cast<int>(std::fmod(hour_, 1.0f) * 60));
        ui_.begin_panel(kGoalPanel, line);
        usize homes = 0, works = 0, shops = 0;
        for (const Building& b : buildings_) (b.kind == kHome ? homes : b.kind == kWork ? works : shops)++;
        std::snprintf(line, sizeof line, "%zu homes, %zu jobs, %zu shops", homes, works, shops);
        ui_.label(line);
        std::snprintf(line, sizeof line, "%zu cars  %u ok  %u late  %u lost", cars_.count(),
                      trips_served_ - trips_late_, trips_late_, trips_lost_);
        ui_.label(line, true);
        const usize open_routes = routes_.size() - routes_connected();
        std::snprintf(line, sizeof line, "%zu of %zu exits linked", routes_.size() - open_routes, routes_.size());
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
            const Vec2 at = view(view_center(), zoom_).to_screen(tip);
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
        const Rect undo_box{kShapeStrip.x, kShapeStrip.y + 50, kShapeStrip.w, 21};
        char undo_label[32];
        std::snprintf(undo_label, sizeof undo_label, "Undo (%zu)", history_.size());
        if (ui_.button_at(undo_box, undo_label) && !history_.empty()) app_->push_command(kCmdUndo);
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
    Vec2 zoom_anchor_{kWidth * 0.5f, kHeight * 0.5f};
    std::vector<Island> islands_;
    std::vector<BeforeImage> history_; ///< what to put back, newest last
    std::vector<u32> pockets_; ///< edges that carry a turn pocket, for the taper marking
    bool auto_pockets_ = true;
    std::vector<Signal> signals_; ///< kept sorted by node
    std::vector<TownAccess> access_;
    u64 signal_version_ = 0;      ///< bumped whenever a signal is added, removed or re-phased
    std::vector<RouteDemand> routes_;
    std::vector<Building> buildings_;
    std::vector<TownAccess> doors_; ///< each building's way onto the network
    float hour_ = 7.5f;             ///< time of day, 0..24
    float grow_timer_ = 0;
    u32 trips_served_ = 0;
    u32 trips_late_ = 0;
    u32 trips_lost_ = 0;
    u64 access_version_ = ~0ull;
    std::vector<float> flow_; ///< per lane, 1 = free running, 0 = stopped
    float money_ = 20000;
    float spent_ = 0;
    bool show_flow_ = false;
    float clock_ = 0;    ///< seconds since the map started
    float demand_ = 3.0f;       ///< through trips per minute for a pair of size-1 exits
    float local_demand_ = 2.4f; ///< trips per minute per building floor at its peak
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
    int demo_ = 0;
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
    std::vector<u8> seam_shift_; ///< per node: a seam whose lanes shift, rebuilt with the meshes
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
