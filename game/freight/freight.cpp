// Spedition: a small rail and road freight company on the Rewind engine.
//
// Grandfather left a yard on a branch line, a filling station at the gate,
// two vehicles and a loan that costs more each month than one load pays.
// Gravel comes off the hillside above Oberdorf and the valley wants it.
// Send the lorry or send the train, watch the run, and see whether it paid
// for itself once the diesel and the standing charges are off it.
//
// This is phase 1 of game/freight/README.md: the line runs. The physical
// model that makes the choice interesting -- mass against grade, fuel for
// work actually done, wear where the work happens -- is phase 2. What is
// here is the machinery it hangs on, and the look it hangs in.
//
// Everything the game simulates -- both networks, the fleet, the calendar,
// the money and the camera -- is reflected state on the fixed step, so the
// whole company rewinds with F4 and replays with F5.
//
// Two scales meet in this file and it is worth being plain about them.
//
//   Drawn metres are what the networks, the camera and the vehicle bodies
//   are in. The valley is about 1.4 km of them across.
//   Real metres are what the company is in: the branch is fifteen kilometres
//   of railway, a lorry does 85 km/h, gravel is priced by the real tonne.
//   kMapScale is the one place they convert.
//
//   Simulated seconds are the fixed step. Game seconds are what the calendar
//   counts, and it runs faster; Clock::rate() is the one place those convert.
//
// A speed therefore crosses both: a real km/h becomes drawn metres per
// simulated second in params_for(), and nowhere else. Vehicle bodies are the
// deliberate exception -- they are drawn at their real size so they are
// recognisable, which makes them enormous against a map this compressed, the
// way a lorry on a road atlas is.
//
// Money is euros. Tonnes are tonnes.

#include "style.h"

#include <rewind/rewind.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace rw;
using namespace freight;

namespace {

/// A fixed window, like the traffic game: the simulation turns recorded
/// mouse pixels into world positions itself, so a replay has to see the
/// same view it was recorded with.
constexpr float kWidth = 1440;
constexpr float kHeight = 810;
constexpr u32 kNone = ~0u;

/// Real metres to one drawn metre. The drawn branch is 1.25 km; the railway
/// it stands for is fifteen.
constexpr float kMapScale = 12.0f;

/// Simulated seconds a game hour takes. At 90 a day is 36 minutes at normal
/// speed and nine at 4x, and a lorry's round trip reads as a morning's work
/// while taking a couple of minutes to watch.
constexpr float kHourSeconds = 90.0f;

/// UI clicks arrive as commands inside the deterministic input record.
constexpr u32 kCmdDispatch = 1; ///< value: index into the fleet

// ---- the company it starts as -----------------------------------------------

constexpr double kStartMoney = 8400;   ///< what is actually in the account
constexpr double kStartLoan = 42000;   ///< what grandfather signed for
constexpr double kStandingCost = 165;  ///< a day of rent, rates and insurance
constexpr double kLoanRatePerDay = 0.06 / 365.0;

/// There is one customer and one hopper, so there is a limit to what the
/// valley will take in a day. Past it the stuff still moves, but it goes at
/// a price that does not reward running the fleet into the ground. This is
/// the placeholder for contracts, which arrive in phase 3.
constexpr float kOverQuotaRate = 0.4f;

// ---- the view ---------------------------------------------------------------

constexpr float kStartZoom = 1.0f;
constexpr float kMinZoom = 0.30f;
constexpr float kMaxZoom = 8.0f;
constexpr float kDetailZoom = 2.2f; ///< above this the track and road draw their detail
constexpr float kPanSpeed = 380;    ///< screen pixels a second, so it feels the same at any zoom

/// Monday is 0, because that is where the week starts here.
const char* const kWeekdays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
const char* const kWayNames[] = {"Road", "Rail"};

// ---- data tables ------------------------------------------------------------

/// A vehicle the company can own. Rows in assets/vehicles.json, hot reloaded.
/// Phase 2 replaces `cost_per_km` with fuel burnt for work done and wear
/// booked against the component that did the work.
struct VehicleDef {
    std::string id;
    std::string name;
    u32 rail = 0; ///< 0 = road, 1 = rail
    float length_m = 12;
    float width_m = 2.5f;
    float tare_t = 7.5f;
    float capacity_t = 18;
    float top_speed_kmh = 80;
    float accel = 0.9f;
    float decel = 1.8f;
    float cost_per_km = 0.85f;
    float price = 0;

    void reflect(Visitor& v) {
        v.field("id", id);
        v.field("name", name);
        v.field("rail", rail, {.min = 0, .max = 1, .names = kWayNames, .name_count = 2});
        v.field("length_m", length_m, {.min = 1});
        v.field("width_m", width_m, {.min = 0.5f});
        v.field("tare_t", tare_t, {.min = 0});
        v.field("capacity_t", capacity_t, {.min = 0});
        v.field("top_speed_kmh", top_speed_kmh, {.min = 1});
        v.field("accel", accel, {.min = 0.01f});
        v.field("decel", decel, {.min = 0.01f});
        v.field("cost_per_km", cost_per_km, {.min = 0});
        v.field("price", price, {.min = 0});
    }
};

/// What is being moved. One row for now; the second one is what makes the
/// fleet planning interesting, and it arrives with contracts in phase 3.
struct CargoDef {
    std::string id;
    std::string name;
    float rate_per_t = 4.8f;        ///< what the customer pays to move one tonne
    float tonnes_per_minute = 9.0f; ///< how fast the yard's kit handles it
    float daily_tonnes = 260;       ///< what the valley takes in a day at full price

    void reflect(Visitor& v) {
        v.field("id", id);
        v.field("name", name);
        v.field("rate_per_t", rate_per_t, {.min = 0});
        v.field("tonnes_per_minute", tonnes_per_minute, {.min = 0.1f});
        v.field("daily_tonnes", daily_tonnes, {.min = 0});
    }
};

// ---- a vehicle on the day it is having --------------------------------------

/// Where a vehicle is in its round trip. A run is: out empty, load, back
/// loaded, unload, paid.
enum class Leg : u8 { Idle, Out, Loading, Back, Unloading };
const char* const kLegNames[] = {"in the yard", "running out", "loading", "running back", "unloading"};
constexpr u32 kLegCount = 5;

struct Vehicle {
    std::string def;   ///< id in the vehicles table
    u32 agent = kNone; ///< index in its network's agents, or kNone while parked
    Leg leg = Leg::Idle;
    float leg_time = 0;
    Timer handling;  ///< loading or unloading, counted in simulated seconds
    float load_t = 0;
    float run_m = 0;    ///< metres run this round trip, for the running cost
    float last_net = 0; ///< what the last completed run cleared, after costs
    u32 trips = 0;
    Vec2 pos;
    float heading = 0;

    void reflect(Visitor& v) {
        v.field("def", def);
        v.field("agent", agent);
        v.field("leg", leg, {.names = kLegNames, .name_count = kLegCount});
        v.field("leg_time", leg_time, {.min = 0});
        v.field("handling", handling);
        v.field("load_t", load_t, {.min = 0});
        v.field("run_m", run_m, {.min = 0});
        v.field("last_net", last_net);
        v.field("trips", trips);
        v.field("pos", pos);
        v.field("heading", heading);
    }
};

/// The first track of an edge running with, or against, the edge's own
/// direction. Track order is the network's business, so ask rather than
/// assume index 0 is the one going out.
u32 track_of(const CurveNetwork& net, u32 edge, bool forward) {
    const NetEdge& e = net.edge(edge);
    for (usize i = 0; i < e.tracks.size(); ++i) {
        if (e.tracks[i].forward == forward) return static_cast<u32>(i);
    }
    return 0;
}

class Freight : public Game {
public:
    // ---- setup --------------------------------------------------------------

    void configure(AppConfig& config) override {
        config.title = "Spedition";
        config.width = static_cast<int>(kWidth);
        config.height = static_cast<int>(kHeight);
        config.resizable = false;
        for (usize i = 0; i + 1 < config.args.size(); ++i) {
            if (config.args[i] == "--zoom") start_zoom_ = std::strtof(config.args[i + 1].c_str(), nullptr);
        }
    }

    void start(App& app) override {
        app_ = &app;
        std::string error;
        if (!vehicles_.load(&error)) {
            RW_LOG_WARN("freight", "vehicles: %s", error.c_str());
            vehicles_.add_row("l608d");
            vehicles_.add_row("v60");
            if (VehicleDef* d = vehicles_.find("v60")) { // enough to still be a railway
                d->rail = 1;
                d->length_m = 58;
                d->capacity_t = 90;
                d->top_speed_kmh = 60;
            }
        }
        if (!cargo_.load(&error)) {
            RW_LOG_WARN("freight", "cargo: %s", error.c_str());
            cargo_.add_row("gravel");
        }
    }

    /// Drop IBM Plex Sans and Mono into assets/fonts/ and the game picks them
    /// up here; without them it draws in the engine's built-in face, which is
    /// legible but is not the look STYLE.md asks for.
    void load_assets(Renderer2D& renderer, Audio&) override {
        body_ = renderer.load_font("fonts/IBMPlexSans-Regular.ttf", 18);
        figures_ = renderer.load_font("fonts/IBMPlexMono-Regular.ttf", 24);
    }

    void init() override {
        build_valley();
        seed_fleet();

        clock_.set_start(1978, 4, 3, 6.0f); // a Monday, six in the morning
        clock_.hour_seconds = kHourSeconds;
        money_ = kStartMoney;
        loan_ = kStartLoan;
        day_open_ = money_;
        last_day_ = 0;
        delivered_t_ = 0;
        day_delivered_t_ = 0;

        money_shown_.duration = kNumberTime;
        money_shown_.snap(static_cast<float>(money_));

        zoom_ = start_zoom_ > 0 ? start_zoom_ : kStartZoom;
        zoom_target_ = zoom_;
        centre_ = kValleyCentre;
        centre_target_ = centre_;
        camera_ = Camera::centered(centre_, zoom_, kWidth, kHeight);
    }

    std::vector<DataTableBase*> data_tables() override { return {&vehicles_, &cargo_}; }

    // ---- the tick -----------------------------------------------------------

    void tick(const TickContext& ctx) override {
        const ClockEdge edge = clock_.tick(ctx.dt);
        if (edge.day) close_the_day();

        if (ctx.input.command == kCmdDispatch) {
            const u32 index = static_cast<u32>(ctx.input.command_value);
            if (index < fleet_.size()) dispatch(index);
        }

        for (u32 i = 0; i < fleet_.size(); ++i) run_leg(i, ctx.dt);
        rail_agents_.tick(rail_, ctx.dt);
        road_agents_.tick(road_, ctx.dt);
        for (Vehicle& v : fleet_) follow_agent(v, ctx.dt);

        move_camera(ctx);

        // Nothing the eye can follow jumps: the figure counts to the balance.
        money_shown_.set(static_cast<float>(money_));
        money_shown_.tick(ctx.dt);
    }

    void reflect(Visitor& v) override {
        v.field("rail", rail_);
        v.field("road", road_);
        v.field("rail_agents", rail_agents_);
        v.field("road_agents", road_agents_);
        v.field("fleet", fleet_);
        v.field("clock", clock_);
        v.field("money", money_);
        v.field("loan", loan_, {.min = 0});
        v.field("day_open", day_open_);
        v.field("last_day", last_day_);
        v.field("delivered_t", delivered_t_, {.min = 0});
        v.field("day_delivered_t", day_delivered_t_, {.min = 0});
        v.field("money_shown", money_shown_);
        v.field("camera", camera_);
        v.field("centre", centre_);
        v.field("centre_target", centre_target_);
        v.field("zoom", zoom_, {.min = kMinZoom, .max = kMaxZoom});
        v.field("zoom_target", zoom_target_, {.min = kMinZoom, .max = kMaxZoom});
        v.field("rail_first", rail_first_);
        v.field("rail_last", rail_last_);
        v.field("road_first", road_first_);
        v.field("road_last", road_last_);
    }

    // ---- drawing ------------------------------------------------------------

    void render(Renderer2D& r, float) override {
        r.clear(kGround);
        r.set_camera(camera_);
        draw_ground(r);
        draw_rail(r);
        draw_road(r);
        draw_places(r);
        draw_fleet(r);

        r.set_camera(Camera{}); // the shell is in screen pixels
        draw_place_labels(r);
        draw_shell(r);
    }

private:
    // ---- the valley ---------------------------------------------------------

    static constexpr Vec2 kYard{190, 650};
    static constexpr Vec2 kQuarry{1300, 285};
    static constexpr Vec2 kValleyCentre{720, 470};

    /// Rail and road are separate networks on purpose. The engine routes one
    /// class of vehicle per network -- track penalties are global, not per
    /// vehicle -- so one network carrying both would need a per-class routing
    /// cost that does not exist yet. Two networks is what the engine supports
    /// today, and the yard is where they meet.
    void build_valley() {
        rail_.clear();
        road_.clear();

        // The branch: out of the yard along the valley floor, then up to the
        // quarry. Elevation is carried now and paid for in phase 2.
        const u32 r0 = rail_.add_node(kYard, 0, 0, 12);
        const u32 r1 = rail_.add_node({640, 596}, 7, 0, 12);
        const u32 r2 = rail_.add_node({1010, 470}, 15, 0, 12);
        const u32 r3 = rail_.add_node(kQuarry, 24, 0, 12);
        const u32 re0 = rail_.add_curved_edge(r0, r1, {1.0f, -0.05f}, {1.0f, -0.18f});
        const u32 re1 = rail_.add_curved_edge(r1, r2, {1.0f, -0.18f}, {1.0f, -0.42f});
        const u32 re2 = rail_.add_curved_edge(r2, r3, {1.0f, -0.42f}, {1.0f, -0.62f});
        for (const u32 e : {re0, re1, re2}) {
            rail_.set_tracks(e, 1, 1, kTrackSpacing);
            rail_.edge(e).width = 7.0f;
        }
        rail_.rebuild();
        rail_first_ = re0;
        rail_last_ = re2;

        // The road takes the long way round the shoulder of the hill.
        const u32 d0 = road_.add_node({240, 700}, 0, 0, 9);
        const u32 d1 = road_.add_node({610, 742}, 4, 0, 9);
        const u32 d2 = road_.add_node({1035, 616}, 14, 0, 9);
        const u32 d3 = road_.add_node({1332, 355}, 24, 0, 9);
        const u32 de0 = road_.add_curved_edge(d0, d1, {1.0f, 0.25f}, {1.0f, 0.0f});
        const u32 de1 = road_.add_curved_edge(d1, d2, {1.0f, 0.0f}, {1.0f, -0.45f});
        const u32 de2 = road_.add_curved_edge(d2, d3, {1.0f, -0.45f}, {0.7f, -1.0f});
        for (const u32 e : {de0, de1, de2}) {
            road_.set_tracks(e, 1, 1, kLaneWidth);
            road_.edge(e).width = 8.0f;
        }
        road_.rebuild();
        road_first_ = de0;
        road_last_ = de2;
    }

    void seed_fleet() {
        fleet_.clear();
        add_vehicle("l608d");
        add_vehicle("v60");
        for (Vehicle& v : fleet_) v.pos = def_of(v).rail ? kYard : Vec2{240, 700};
    }

    void add_vehicle(const char* id) {
        Vehicle v;
        v.def = id;
        fleet_.push_back(v);
    }

    /// A row that is always there, so a missing or broken table degrades to a
    /// drivable lorry instead of a crash.
    const VehicleDef& def_of(const Vehicle& v) const {
        static const VehicleDef fallback;
        const VehicleDef* d = vehicles_.find(v.def);
        return d ? *d : fallback;
    }
    const CargoDef& cargo() const {
        static const CargoDef fallback;
        const CargoDef* c = cargo_.size() ? &cargo_.at(0) : nullptr;
        return c ? *c : fallback;
    }

    // ---- running a job ------------------------------------------------------

    /// The one place a real speed becomes a drawn one. A metre per real
    /// second is 1/kMapScale drawn metres, and a real second is rate()
    /// simulated ones, so a rate of change picks up one factor of each and an
    /// acceleration picks up two.
    NetAgentParams params_for(const VehicleDef& d) const {
        const float r = static_cast<float>(clock_.rate());
        NetAgentParams p;
        p.length = d.length_m; // drawn at its real size, on purpose
        p.max_speed = (d.top_speed_kmh / 3.6f) * r / kMapScale;
        p.accel = d.accel * r * r / kMapScale;
        p.decel = d.decel * r * r / kMapScale;
        p.min_gap = d.rail ? 24.0f : 6.0f;
        p.headway = d.rail ? 1.5f : 0.8f;
        return p;
    }

    Route plan(const CurveNetwork& net, u32 from_edge, u32 to_edge, bool outward) const {
        return net.route(TrackRef{from_edge, track_of(net, from_edge, outward)},
                         TrackRef{to_edge, track_of(net, to_edge, outward)});
    }

    /// Put a vehicle on the network for one leg. Outward runs yard to quarry,
    /// homeward runs the other way on the other track.
    bool launch(Vehicle& v, u32 index, bool outward) {
        const VehicleDef& d = def_of(v);
        CurveNetwork& net = d.rail ? rail_ : road_;
        NetworkAgents& agents = d.rail ? rail_agents_ : road_agents_;
        const u32 first = d.rail ? rail_first_ : road_first_;
        const u32 last = d.rail ? rail_last_ : road_last_;
        const Route route = outward ? plan(net, first, last, true) : plan(net, last, first, false);
        if (!route.found) return false;
        v.agent = agents.spawn(route, 0, params_for(d), index);
        return true;
    }

    void dispatch(u32 index) {
        Vehicle& v = fleet_[index];
        if (v.leg != Leg::Idle) return;
        if (!launch(v, index, true)) return;
        v.leg = Leg::Out;
        v.leg_time = 0;
        v.run_m = 0;
        v.load_t = 0;
    }

    /// Handling takes game time -- a shovel does not care how fast the clock
    /// is set -- so the answer comes back in simulated seconds.
    float handling_seconds(const VehicleDef& d) const {
        const float per_minute = cargo().tonnes_per_minute;
        const float game_seconds = per_minute > 0 ? (d.capacity_t / per_minute) * 60.0f : 600.0f;
        const float rate = static_cast<float>(clock_.rate());
        return rate > 0 ? game_seconds / rate : game_seconds;
    }

    /// Drawn metres run, as the real kilometres the costs are priced in.
    static float real_km(float drawn_m) { return drawn_m * kMapScale / 1000.0f; }

    void run_leg(u32 index, float dt) {
        Vehicle& v = fleet_[index];
        const VehicleDef& d = def_of(v);
        NetworkAgents& agents = d.rail ? rail_agents_ : road_agents_;
        v.leg_time += dt;

        switch (v.leg) {
        case Leg::Idle:
            break;

        case Leg::Out:
        case Leg::Back: {
            if (v.agent == kNone || !agents.valid(v.agent)) { // the network changed under it
                v.leg = Leg::Idle;
                v.agent = kNone;
                break;
            }
            const NetAgent& a = agents.agent(v.agent);
            v.run_m += a.speed * dt;
            if (!a.done) break;

            const bool was_out = v.leg == Leg::Out;
            agents.remove(v.agent);
            v.agent = kNone;
            v.leg = was_out ? Leg::Loading : Leg::Unloading;
            v.leg_time = 0;
            v.handling.start(handling_seconds(d));
            break;
        }

        case Leg::Loading: {
            if (!v.handling.tick(dt)) break;
            v.load_t = d.capacity_t;
            if (launch(v, index, false)) {
                v.leg = Leg::Back;
                v.leg_time = 0;
            } else {
                v.leg = Leg::Idle; // nowhere to go; it sits at the quarry
            }
            break;
        }

        case Leg::Unloading: {
            if (!v.handling.tick(dt)) break;
            const float room = cargo().daily_tonnes - day_delivered_t_;
            const float at_rate = clampf(v.load_t, 0.0f, room > 0 ? room : 0.0f);
            const float over = v.load_t - at_rate;
            const double paid = (static_cast<double>(at_rate) + static_cast<double>(over) * kOverQuotaRate)
                                * static_cast<double>(cargo().rate_per_t);
            const double running = static_cast<double>(real_km(v.run_m)) * d.cost_per_km;
            money_ += paid - running;
            delivered_t_ += v.load_t;
            day_delivered_t_ += v.load_t;
            v.last_net = static_cast<float>(paid - running);
            v.load_t = 0;
            v.run_m = 0;
            ++v.trips;
            v.leg = Leg::Idle;
            v.leg_time = 0;
            break;
        }
        }
    }

    /// Where a vehicle is drawn comes from its agent while it has one, and
    /// stays where it was left while it does not.
    void follow_agent(Vehicle& v, float) {
        if (v.agent == kNone) return;
        const VehicleDef& d = def_of(v);
        const NetworkAgents& agents = d.rail ? rail_agents_ : road_agents_;
        const CurveNetwork& net = d.rail ? rail_ : road_;
        if (!agents.valid(v.agent)) return;
        const PathPoint p = agents.pose(net, v.agent);
        v.pos = p.position;
        v.heading = angle_of(p.direction);
    }

    void close_the_day() {
        const double interest = loan_ * kLoanRatePerDay;
        money_ -= kStandingCost + interest;
        last_day_ = static_cast<float>(money_ - day_open_);
        day_open_ = money_;
        day_delivered_t_ = 0;
    }

    // ---- the view -----------------------------------------------------------

    void move_camera(const TickContext& ctx) {
        Vec2 pan{};
        if (ctx.input.down(Button::Left)) pan.x -= 1;
        if (ctx.input.down(Button::Right)) pan.x += 1;
        if (ctx.input.down(Button::Up)) pan.y -= 1;
        if (ctx.input.down(Button::Down)) pan.y += 1;
        if (pan.x != 0 || pan.y != 0) {
            centre_target_ += normalize(pan) * (kPanSpeed * ctx.dt / zoom_target_);
        }
        if (ctx.input.wheel != 0) {
            const float step = std::pow(1.15f, static_cast<float>(ctx.input.wheel));
            const float was = zoom_target_;
            zoom_target_ = clampf(zoom_target_ * step, kMinZoom, kMaxZoom);
            // Zoom around the cursor, so what you were looking at is what you
            // get more of. The world point under the mouse is
            // centre + offset/zoom; hold it still and the centre has to move.
            const Vec2 mouse{static_cast<float>(ctx.input.mouse_x), static_cast<float>(ctx.input.mouse_y)};
            if (mouse.x >= 0 && mouse.y >= 0 && zoom_target_ != was) {
                const Vec2 offset = mouse - Vec2{kWidth * 0.5f, kHeight * 0.5f};
                centre_target_ += offset * (1.0f / was - 1.0f / zoom_target_);
            }
        }
        // The camera eases to where it was told to be, and never snaps there.
        zoom_ = approach(zoom_, zoom_target_, kCameraRate, ctx.dt);
        centre_ = approach(centre_, centre_target_, kCameraRate, ctx.dt);
        camera_ = Camera::centered(centre_, zoom_, kWidth, kHeight);
    }

    /// A world width that is never thinner than `pixels` on screen, so a rail
    /// does not vanish when the valley is drawn small.
    float at_least(float metres, float pixels) const {
        const float floor_m = pixels / camera_.zoom;
        return metres > floor_m ? metres : floor_m;
    }

    // ---- the world ----------------------------------------------------------

    void draw_ground(Renderer2D& r) const {
        // Two quiet bands: the valley floor, and the shoulder the line climbs.
        r.fill_rect(-400, 240, 2400, 900, kGrass);
        const std::vector<Vec2> hill = {{560, -200}, {2000, -200}, {2000, 470}, {1180, 355}, {820, 250}};
        r.fill_polygon(hill, kSurface);
        r.draw_polyline({{560, -200}, {820, 250}, {1180, 355}, {2000, 470}},
                        stroke(at_least(kHairline * 6, 1.0f), kLine), nullptr);
    }

    void draw_rail(Renderer2D& r) const {
        const bool detail = camera_.zoom >= kDetailZoom;
        for (const NetEdge& e : rail_.edges()) {
            if (!e.alive) continue;
            r.draw_path(e.path, stroke(7.0f, kBallast), 0.4f);
        }
        for (usize i = 0; i < rail_.track_count(); ++i) {
            const TrackRef t = rail_.track_ref(static_cast<u32>(i));
            if (!rail_.valid_edge(t.edge)) continue;
            const Path& p = rail_.track_path(t);
            if (!detail) {
                r.draw_path(p, stroke(at_least(kGauge, 1.6f), kMuted), 0.4f);
                continue;
            }
            // Two rails: a band the gauge wide, with the ballast showing
            // between them.
            r.draw_path(p, stroke(kGauge, kMuted), 0.25f);
            r.draw_path(p, stroke(kGauge - 2 * kStandard, kBallast), 0.25f);
        }
    }

    void draw_road(Renderer2D& r) const {
        const bool detail = camera_.zoom >= kDetailZoom;
        for (const NetEdge& e : road_.edges()) {
            if (!e.alive) continue;
            r.draw_path(e.path, stroke(at_least(2 * kLaneWidth + 1.2f, 2.5f), kAsphalt), 0.4f);
            if (!detail) continue;
            r.draw_path(e.path, stroke(kStandard, kLine), 0.25f);
        }
    }

    void draw_places(Renderer2D& r) const {
        // The yard: hardstanding, the shed, the tank, and the filling station
        // canopy out on the road side.
        r.fill_rect(kYard.x - 70, kYard.y - 46, 250, 118, kSurface);
        r.draw_rect(kYard.x - 70, kYard.y - 46, 250, 118, kLine);
        r.fill_rect(kYard.x - 46, kYard.y - 34, 88, 40, kInk);
        r.fill_rect(kYard.x + 62, kYard.y - 30, 34, 34, kMuted);
        r.fill_rect(kYard.x + 4, kYard.y + 30, 96, 30, kAccent);
        r.fill_rect(kYard.x + 20, kYard.y + 38, 22, 14, kSurface);
        r.fill_rect(kYard.x + 56, kYard.y + 38, 22, 14, kSurface);

        // The quarry: a bench cut into the hill, and the loading hopper.
        r.fill_rect(kQuarry.x - 96, kQuarry.y - 58, 190, 122, kBallast);
        r.draw_rect(kQuarry.x - 96, kQuarry.y - 58, 190, 122, kLine);
        r.fill_rect(kQuarry.x - 30, kQuarry.y - 40, 60, 44, kMuted);
    }

    void draw_fleet(Renderer2D& r) const {
        for (const Vehicle& v : fleet_) {
            const VehicleDef& d = def_of(v);
            const float length = at_least(d.length_m, 6.0f);
            const float width = at_least(d.width_m, 2.6f);
            const Vec2 forward = from_angle(v.heading);
            const Vec2 side = perp(forward);

            const std::vector<Vec2> body = corners(v.pos + Vec2{0, width * 0.35f}, forward, side, length, width);
            r.fill_polygon(body, kShadow);
            r.fill_polygon(corners(v.pos, forward, side, length, width), kInk);

            // Loaded shows: the deck goes the colour of the cargo it carries.
            if (v.load_t > 0) {
                const float deck = length * (d.rail ? 0.82f : 0.62f);
                const Vec2 at = v.pos - forward * (length * (d.rail ? 0.0f : 0.16f));
                r.fill_polygon(corners(at, forward, side, deck, width * 0.62f), kGood);
            }
            // The nose, so it is obvious which way it is pointing.
            r.fill_polygon(corners(v.pos + forward * (length * 0.42f), forward, side, length * 0.16f, width),
                           kAccent);
        }
    }

    static std::vector<Vec2> corners(Vec2 at, Vec2 forward, Vec2 side, float length, float width) {
        const Vec2 h = forward * (length * 0.5f);
        const Vec2 w = side * (width * 0.5f);
        return {at - h - w, at + h - w, at + h + w, at - h + w};
    }

    // ---- the shell ----------------------------------------------------------

    UiStyle shell_style() const {
        UiStyle s;
        s.panel = kSurface;
        s.panel_border = kLine;
        s.title = kInk;
        s.text = kInk;
        s.text_dim = kMuted;
        s.button = kSurface;
        s.button_hover = rgb(0xE7, 0xE2, 0xD8);
        s.button_active = kAccent;
        s.button_border = kLine;
        s.accent = kAccent;
        s.track = kLine;
        s.tooltip = kInk;
        s.padding = kPad;
        s.spacing = kGap;
        s.item_height = kRow;
        s.rounding = kRadius;
        s.pixel_font_scale = 1;
        return s;
    }

    void draw_place_labels(Renderer2D& r) const {
        label_at(r, kYard + Vec2{0, -60}, "YARD", "0 m");
        label_at(r, kQuarry + Vec2{0, -72}, "OBERDORF QUARRY", "24 m");
    }

    void label_at(Renderer2D& r, Vec2 world, const char* name, const char* height) const {
        const Vec2 p = camera_.to_screen(world);
        if (p.x < -200 || p.x > kWidth + 200 || p.y < -80 || p.y > kHeight + 80) return;
        draw_text_centred(r, p, name, kInk, kSmallSize);
        draw_text_centred(r, p + Vec2{0, kUnit * 2}, height, kMuted, kSmallSize);
    }

    void draw_shell(Renderer2D& r) const {
        if (!app_) return;
        char line[128];

        ui_.begin(&r, app_->ui_input(), body_ && body_->ok ? body_ : nullptr, shell_style());

        // A Ui row advances by its height plus the spacing under it, and a
        // panel adds its padding twice and its title once. Sizing the panels
        // from that rather than from a guess keeps the last line inside.
        const float pitch = kRow + kGap;
        const float head = kPad * 2 + ui_.line_height() + kGap - kGap;
        const float fleet_h = head + static_cast<float>(fleet_.size()) * (3 * pitch + kGap);
        const float job_h = head + 4 * pitch;

        // The top bar: what day it is, and what the company is worth.
        ui_.begin_panel(Rect{0, 0, kWidth, kPad * 2 + kRow});
        std::snprintf(line, sizeof line, "%s  %02u.%02u.%d      %02u:%02u", kWeekdays[clock_.weekday()],
                      clock_.date().day, clock_.date().month, clock_.date().year, clock_.hour(),
                      clock_.minute());
        ui_.label(line);
        ui_.end_panel();

        // The fleet: one row a vehicle, and the one button phase 1 has.
        ui_.begin_panel(Rect{kPad, kPad * 2 + kRow + kPad, 336, fleet_h}, "Fleet");
        for (u32 i = 0; i < fleet_.size(); ++i) {
            const Vehicle& v = fleet_[i];
            const VehicleDef& d = def_of(v);
            std::snprintf(line, sizeof line, "%s  %s", d.name.c_str(), kWayNames[d.rail ? 1 : 0]);
            ui_.label(line);
            if (v.leg == Leg::Idle) {
                std::snprintf(line, sizeof line, "%u trips, last %+.0f EUR", v.trips, v.last_net);
                ui_.label(line, true);
                if (ui_.button("Dispatch", 140)) app_->push_command(kCmdDispatch, static_cast<i32>(i));
            } else if (v.leg == Leg::Loading || v.leg == Leg::Unloading) {
                std::snprintf(line, sizeof line, "%s  %.0f t", kLegNames[static_cast<u32>(v.leg)],
                              v.leg == Leg::Loading ? d.capacity_t : v.load_t);
                ui_.label(line, true);
                ui_.progress(v.handling.progress(), 140, kAccent);
            } else {
                std::snprintf(line, sizeof line, "%s  %.1f km", kLegNames[static_cast<u32>(v.leg)],
                              real_km(v.run_m));
                ui_.label(line, true);
                ui_.spacing(pitch); // hold the row the button and the bar occupy
            }
            if (i + 1 < fleet_.size()) ui_.spacing(kGap);
        }
        ui_.end_panel();

        // The standing job, until contracts arrive in phase 3.
        ui_.begin_panel(Rect{kWidth - kPad - 420, kHeight - kPad - job_h, 420, job_h}, "Standing job");
        std::snprintf(line, sizeof line, "%s   Oberdorf quarry to the yard", cargo().name.c_str());
        ui_.label(line);
        std::snprintf(line, sizeof line, "%.2f EUR a tonne up to %.0f t a day, then %.0f%%",
                      cargo().rate_per_t, cargo().daily_tonnes, kOverQuotaRate * 100);
        ui_.label(line, true);
        std::snprintf(line, sizeof line, "today %.0f / %.0f t        %.0f t all told", day_delivered_t_,
                      cargo().daily_tonnes, delivered_t_);
        ui_.label(line, true);
        ui_.progress(cargo().daily_tonnes > 0 ? day_delivered_t_ / cargo().daily_tonnes : 0, 388,
                     day_delivered_t_ >= cargo().daily_tonnes ? kWarn : kGood);
        ui_.end_panel();

        ui_.end();

        // The money, in mono, counted rather than cut.
        std::snprintf(line, sizeof line, "%s%.0f EUR", money_shown_.value < 0 ? "-" : "",
                      std::fabs(money_shown_.value));
        draw_text_right(r, {kWidth - kPad, 12}, line, money_ < 0 ? kBad : kInk, kFigureSize, true);
        std::snprintf(line, sizeof line, "loan %.0f    yesterday %+.0f", loan_, last_day_);
        draw_text_right(r, {kWidth - kPad, 38}, line, kMuted, kSmallSize);
    }

    // ---- text ---------------------------------------------------------------
    // The built-in face is a 6x8 cell at integer scales; a real font, when one
    // is present, draws at any size. Both go through here so the shell does
    // not care which it got.

    float text_width(const char* s, float size, bool figures) const {
        const Font* f = figures ? figures_ : body_;
        if (f && f->ok) return f->measure(s, size / f->size()).x;
        return static_cast<float>(std::strlen(s)) * 6.0f * static_cast<float>(pixel_scale(size));
    }

    static int pixel_scale(float size) { return size >= 24 ? 3 : (size >= 15 ? 2 : 1); }

    void draw_text_at(Renderer2D& r, Vec2 at, const char* s, Color c, float size, bool figures) const {
        const Font* f = figures ? figures_ : body_;
        if (f && f->ok) {
            r.draw_text(*f, at.x, at.y, s, c, size / f->size());
            return;
        }
        r.draw_text(at.x, at.y, s, c, pixel_scale(size));
    }

    void draw_text_right(Renderer2D& r, Vec2 at, const char* s, Color c, float size,
                         bool figures = false) const {
        draw_text_at(r, {at.x - text_width(s, size, figures), at.y}, s, c, size, figures);
    }

    void draw_text_centred(Renderer2D& r, Vec2 at, const char* s, Color c, float size) const {
        draw_text_at(r, {at.x - text_width(s, size, false) * 0.5f, at.y}, s, c, size, false);
    }

    // ---- state --------------------------------------------------------------

    CurveNetwork rail_;
    CurveNetwork road_;
    NetworkAgents rail_agents_;
    NetworkAgents road_agents_;
    std::vector<Vehicle> fleet_;

    Clock clock_;
    double money_ = kStartMoney;
    double loan_ = kStartLoan;
    double day_open_ = kStartMoney;
    float last_day_ = 0;
    float delivered_t_ = 0;
    float day_delivered_t_ = 0;
    Tweened<float> money_shown_;

    Camera camera_;
    Vec2 centre_ = kValleyCentre;
    Vec2 centre_target_ = kValleyCentre;
    float zoom_ = kStartZoom;
    float zoom_target_ = kStartZoom;

    u32 rail_first_ = 0;
    u32 rail_last_ = 0;
    u32 road_first_ = 0;
    u32 road_last_ = 0;

    // ---- not simulation state -----------------------------------------------

    DataTable<VehicleDef> vehicles_{"vehicles", "vehicles.json"};
    DataTable<CargoDef> cargo_{"cargo", "cargo.json"};
    App* app_ = nullptr;
    Font* body_ = nullptr;
    Font* figures_ = nullptr;
    mutable Ui ui_;
    float start_zoom_ = 0;
};

} // namespace

REWIND_GAME(Freight)
