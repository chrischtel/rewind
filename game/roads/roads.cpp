// Road builder: a sandbox for laying roads by hand and watching junctions
// make themselves.
//
//   Build     click to start, click to end. The end of one road starts the
//             next; right click stops. Straight or Curve. A curve leaves
//             along the heading it starts with: carried on from a road, that
//             is the road's own; from open ground, the second click sets it.
//             Start on the open end of one road and finish on the open end
//             of another and the two are joined with no kink at either end,
//             as an S, a bend or a hairpin, whichever fits.
//   Move      drag a junction or a road end to move it, the middle of a road
//             to bend it.
//   Remove    click a road.
//   Change    click a road to give it the selected kind of road.
//
// The cursor sticks to what it is near: road ends and junctions first, then
// the middle of a road, which becomes a junction when the road is laid. What
// will be built is shown before the click, with a ring wherever a junction
// will form. Everything is simulation state on the fixed step, the camera
// and the undo history included, so F4 rewinds an edit as it would anything.
//
// Right drag or WASD pans, the wheel zooms about the cursor. --demo 1 starts
// with a few junctions laid out; --zoom and --at X Y set the view.

#include <rewind/rewind.h>

#include "build.h"
#include "paint.h"
#include "signals.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace rw;
using namespace roads;

namespace {

constexpr float kWidth = 1280, kHeight = 720;

/// Commands from the panel. They ride in the input frame, so a click on a
/// button is recorded, rewinds and replays like a key press.
enum Command : u32 { kCmdTool = 1, kCmdShape, kCmdType, kCmdGrid, kCmdAngle, kCmdUndo, kCmdRedo, kCmdClear, kCmdDemo, kCmdLanes, kCmdCars, kCmdResize,
                     // Traffic lights: all of them act on the junction that is selected.
                     kCmdLightPreset, kCmdWalk, kCmdAdaptive, kCmdProgram, kCmdProgramAdd, kCmdProgramRemove, kCmdStart,
                     kCmdPhase, kCmdPhaseAdd, kCmdPhaseRemove, kCmdGreen, kCmdMaxGreen, kCmdAllRed, kCmdClockRate,
                     // Lanes: all of them act on the lane that is selected.
                     kCmdLaneArrow, kCmdLaneAuto, kCmdLaneAdd, kCmdLaneRemove, kCmdLaneFlip, kCmdLanePocket };

enum Tool : u32 { kToolBuild, kToolMove, kToolRemove, kToolChange, kToolLights, kToolLanes, kToolCount };
const char* const kToolNames[kToolCount] = {"Build", "Move", "Remove", "Change", "Lights", "Lanes"};

constexpr i32 kLightsOff = 99;                       ///< kCmdLightPreset: take the lights away
const float kClockRates[] = {60.0f, 600.0f, 1.0f};   ///< game seconds a second: a day in 24 minutes, in 2.4, or in real time
const char* const kClockNames[] = {"x60", "x600", "x1"};
const char* const kPresetNames[] = {"Simple", "Facing", "Lefts", "Custom"};
const Color kLampRed = rgb(232, 64, 54), kLampAmber = rgb(246, 186, 48), kLampGreen = rgb(72, 208, 104);

constexpr float kGridStep = 5.0f;       ///< what clicks snap to on the grid, metres
constexpr float kAngleStep = 15.0f;     ///< degrees
constexpr float kNodeReach = 22.0f;     ///< pixels: how far a junction or road end pulls the cursor
constexpr float kEdgeReach = 15.0f;     ///< pixels: how far the middle of a road does
constexpr float kHold = 1.5f;           ///< once stuck, the cursor has to go this many times further to come free
constexpr usize kUndoDepth = 48;

const Color kGround = rgb(116, 140, 88);
const Color kGridMinor = rgba(255, 255, 255, 14);
const Color kGridMajor = rgba(255, 255, 255, 34);
const Color kGhost = rgba(120, 180, 255, 120);
const Color kGhostBad = rgba(240, 96, 84, 130);
const Color kGhostLine = rgba(240, 246, 255, 200);
const Color kMagnet = rgb(255, 204, 92);
const Color kAccent = rgb(54, 110, 196);
const Color kDanger = rgb(226, 78, 66);

constexpr float kCardWidth = 104;

constexpr float kSpawnEvery = 0.7f;   ///< seconds between cars setting off, somewhere on the map
constexpr usize kMostCars = 260;
constexpr float kCarLength = 4.5f, kCarWidth = 1.9f;
constexpr float kTurnSpeed = 6.5f;    ///< metres a second through a turn
const Color kCarColors[] = {rgb(214, 70, 60),  rgb(60, 110, 200), rgb(236, 236, 230), rgb(52, 56, 64),
                            rgb(232, 180, 50), rgb(70, 150, 110), rgb(150, 156, 166), rgb(120, 76, 150)};

/// What a car needs remembered beside the engine's own agent: where it is on
/// the ground, so it can be put back on the road when the road is rebuilt
/// under it, and where it is going.
struct Car {
    Vec2 pos;
    Vec2 dir{1, 0};
    Vec2 dest;
    void reflect(Visitor& v) {
        v.field("pos", pos);
        v.field("dir", dir);
        v.field("dest", dest);
    }
};

/// One step back in the history: the whole network, as bytes.
struct Snapshot {
    std::vector<u8> bytes;
    void reflect(Visitor& v) { v.field("bytes", bytes); }
};

} // namespace

class Roads final : public Game {
public:
    void configure(AppConfig& cfg) override {
        cfg.title = "Road builder";
        cfg.organization = "Rewind";
        cfg.width = static_cast<int>(kWidth);
        cfg.height = static_cast<int>(kHeight);
        // Fullscreen unless asked otherwise (F11 switches); a scripted run
        // for a screenshot keeps the window it was written for.
        cfg.fullscreen = true;
        for (const std::string& arg : cfg.args)
            if (arg == "--windowed" || arg == "--frames" || arg == "--screenshot") cfg.fullscreen = false;
        for (const std::string& arg : cfg.args)
            if (arg == "--fullscreen") cfg.fullscreen = true;
        cfg.msaa = 4;
        cfg.pixel_art = false;
        cfg.start_with_overlay = false;
        cfg.timeline.tick_rate = 60;
        cfg.timeline.history_ticks = 60 * 60;
        cfg.timeline.keyframe_interval = 20;
        for (usize i = 0; i + 1 < cfg.args.size(); ++i) {
            if (cfg.args[i] == "--demo") start_demo_ = std::stoi(cfg.args[i + 1]) != 0;
            if (cfg.args[i] == "--cars") start_cars_ = std::stoi(cfg.args[i + 1]) != 0;
            if (cfg.args[i] == "--gallery") start_gallery_ = std::stoi(cfg.args[i + 1]) != 0;
            if (cfg.args[i] == "--zoom") start_zoom_ = clampf(std::stof(cfg.args[i + 1]), 0.5f, 30.0f);
            if (cfg.args[i] == "--at" && i + 2 < cfg.args.size()) start_at_ = {std::stof(cfg.args[i + 1]), std::stof(cfg.args[i + 2])};
            if (cfg.args[i] == "--lights" && i + 2 < cfg.args.size()) {
                start_lights_ = true;
                start_lights_at_ = {std::stof(cfg.args[i + 1]), std::stof(cfg.args[i + 2])};
            }
        }
    }

    void start(App& app) override {
        app_ = &app;
        attach();
    }
    void reloaded(App& app) override {
        app_ = &app;
        attach();
    }

    void init() override {
        attach();
        net_.track_tolerance = 0.03f; // metres, and the lanes are what gets drawn: no corners in a bend
        zoom_ = zoom_target_ = start_zoom_;
        cam_center_ = cam_target_ = start_at_;
        if (start_gallery_) {
            build_gallery();
        } else if (start_demo_) {
            build_demo();
            demo_page_ = 1;
        }
        refresh(net_);
        light_up();
        cars_on_ = start_cars_;
        if (start_lights_) {
            // Open the lights editor on the junction nearest a point, with lights on it.
            const u32 node = net_.nearest_node(start_lights_at_, 40);
            if (SignalBox* box = add_lights(net_, signals_, node, kProtected)) {
                set_walk(*box, true);
                net_.invalidate();
                refresh(net_);
                tool_ = kToolLights;
                sel_node_ = node;
            }
        }
    }

    // ---- simulation ---------------------------------------------------------

    void tick(const TickContext& ctx) override {
        const InputFrame& in = ctx.input;
        handle_command(in);
        update_camera(in, ctx.dt);

        const Vec2 world = to_world(in);
        const bool on_panel = over_panel(static_cast<float>(in.mouse_x), static_cast<float>(in.mouse_y));
        const bool down = in.down(Button::MouseLeft), was = prev_input_.down(Button::MouseLeft);
        const bool pressed = down && !was && !on_panel;
        const bool released = !down && was;
        const bool cancel = in.down(Button::MouseRight) && !prev_input_.down(Button::MouseRight);
        cursor_ = world;
        pulse_ += ctx.dt;

        switch (tool_) {
        case kToolBuild: tick_build(world, pressed, cancel, on_panel, ctx.dt); break;
        case kToolMove: tick_move(world, pressed, down, released, on_panel); break;
        case kToolLights: tick_lights(world, pressed); break;
        case kToolLanes: tick_lanes(world, pressed); break;
        default: tick_pick(world, pressed, on_panel); break;
        }
        tick_signals_and_clock(ctx.dt);
        tick_cars(ctx.dt);
        prev_input_ = in;
    }

    void reflect(Visitor& v) override {
        v.field("net", net_);
        v.field("tool", tool_);
        v.field("shape", shape_);
        v.field("type", type_);
        v.field("demo_page", demo_page_);
        v.field("signals", signals_);
        v.field("signals_version", signals_version_);
        v.field("clock", clock_);
        v.field("clock_rate", clock_rate_);
        v.field("sel_node", sel_node_);
        v.field("sel_program", sel_program_);
        v.field("sel_phase", sel_phase_);
        v.field("sel_lane", sel_lane_);
        v.field("hover_lane", hover_lane_);
        v.field("view_w", view_w_);
        v.field("view_h", view_h_);
        v.field("snap_grid", snap_grid_);
        v.field("snap_angle", snap_angle_);
        v.field("show_lanes", show_lanes_);
        v.field("drawing", drawing_);
        v.field("start", start_);
        v.field("target", target_);
        v.field("shown_end", shown_end_);
        v.field("cursor", cursor_);
        v.field("pulse", pulse_);
        v.field("hover_node", hover_node_);
        v.field("hover_edge", hover_edge_);
        v.field("drag_node", drag_node_);
        v.field("drag_edge", drag_edge_);
        v.field("refusal", refusal_);
        v.field("cars_on", cars_on_);
        v.field("cars", cars_);
        v.field("car_info", car_info_);
        v.field("cars_version", cars_version_);
        v.field("spawn_timer", spawn_timer_);
        v.field("rng", rng_);
        v.field("undo", undo_);
        v.field("redo", redo_);
        v.field("cam_center", cam_center_);
        v.field("cam_target", cam_target_);
        v.field("zoom", zoom_);
        v.field("zoom_target", zoom_target_);
        v.field("zoom_anchor", zoom_anchor_);
        v.field("prev_input", prev_input_);
    }

    // ---- rendering ----------------------------------------------------------

    void render(Renderer2D& r, float) override {
        // The window changed size (fullscreen on or off): tell the simulation.
        const int w = r.window_width(), h = r.window_height();
        if (app_ && w > 0 && h > 0 && (static_cast<float>(w) != view_w_ || static_cast<float>(h) != view_h_))
            app_->push_command(kCmdResize, static_cast<i32>((w & 0xFFFF) | ((h & 0x7FFF) << 16)));
        r.clear(kGround);
        r.set_camera(view(view_center(), zoom_));
        draw_grid(r);
        if (painted_ != net_.version() || painted_lights_ != lights_key()) {
            paint_.stop_nodes.clear();
            paint_.walk_nodes.clear();
            for (const SignalBox& box : signals_.boxes) {
                paint_.stop_nodes.push_back(box.node);
                if (box.walk) paint_.walk_nodes.push_back(box.node);
            }
            paint_.rebuild(net_);
            painted_ = net_.version();
            painted_lights_ = lights_key();
        }
        r.draw_mesh(paint_.ground);
        if (show_lanes_) r.draw_mesh(paint_.lanes);
        r.draw_mesh(paint_.paint);
        draw_lamps(r);
        draw_cars(r);
        draw_tool(r);
        draw_panel(r);
    }

private:
    /// Callbacks are not state: they are handed over again after a hot reload.
    void attach() {
        net_.set_connect_filter(may_connect);
        // Where people cross at the lights, the lanes stop further back to leave room for the crossing.
        net_.set_track_trim([this](const CurveNetwork& n, u32 node, u32 edge) {
            const float trim = arm_trim(n, node, edge);
            if (!signals_.walk_at(node)) return trim;
            return std::max(trim, std::min(trim + 4.5f, n.edge(edge).path.length() * 0.45f));
        });
        cars_.set_entry_rule([this](const NetworkAgents& agents, u32 agent, u32 connector) { return may_go(agents, agent, connector); });
        cars_.set_motion_rule([this](const NetworkAgents& agents, const CurveNetwork& net, u32 agent, MotionLimits m) {
            return ease_for_turns(agents, net, agent, m);
        });
    }

    // ---- view -----------------------------------------------------------------

    /// The panel along the bottom. The size of the view is part of the
    /// simulation's state (it arrives as a command when the window changes),
    /// so the simulation can tell by itself whether a click belonged to the
    /// panel or to the world, and a recorded click means the same world point.
    /// The panel and the text are laid out for a screen 720 high and drawn
    /// larger on a bigger one, in whole steps so the lettering stays crisp.
    float ui_scale() const { return std::max(1.0f, std::floor(view_h_ / kHeight)); }
    /// In UI units: screen pixels / ui_scale().
    Rect panel() const { return {view_w_ / ui_scale() * 0.5f - 480, view_h_ / ui_scale() - 116, 960, 106}; }
    /// The lights editor, down the right-hand side while a junction is selected.
    Rect lights_panel() const { return {view_w_ / ui_scale() - 350, 50, 340, 536}; }
    bool lights_open() const { return tool_ == kToolLights && net_.valid_node(sel_node_); }
    /// The lane editor, in the same place while a lane is selected.
    Rect lanes_panel() const { return {view_w_ / ui_scale() - 350, 50, 340, 262}; }
    bool lane_ok(TrackRef lane) const { return net_.valid_edge(lane.edge) && lane.track < net_.edge(lane.edge).tracks.size(); }
    bool lanes_open() const { return tool_ == kToolLanes && lane_ok(sel_lane_); }
    /// The clock, top right: the time of day, and how fast it runs.
    Rect clock_box() const { return {view_w_ / ui_scale() - 170, 12, 160, 28}; }
    bool over_panel(float x, float y) const {
        const float ux = x / ui_scale(), uy = y / ui_scale();
        return panel().contains(ux, uy) || clock_box().contains(ux, uy) || (lights_open() && lights_panel().contains(ux, uy)) ||
               (lanes_open() && lanes_panel().contains(ux, uy));
    }

    Camera view(Vec2 center, float zoom) const { return Camera::centered(center, zoom, view_w_, view_h_); }
    /// The centre the view is drawn from: the smoothed pan, corrected so the
    /// point under the cursor holds still while the zoom catches up with it.
    Vec2 view_center() const {
        const Vec2 off = zoom_anchor_ - Vec2{view_w_ * 0.5f, view_h_ * 0.5f};
        return cam_center_ + off * (1.0f / zoom_target_ - 1.0f / zoom_);
    }
    Vec2 to_world(const InputFrame& in) const {
        return view(view_center(), zoom_).to_world({static_cast<float>(in.mouse_x), static_cast<float>(in.mouse_y)});
    }
    /// Pixels converted to world units at the current zoom.
    float px(float pixels) const { return pixels * ui_scale() / (zoom_ > 0 ? zoom_ : 1); }

    void update_camera(const InputFrame& in, float dt) {
        const float pan_speed = 700.0f / zoom_target_;
        cam_target_.x += static_cast<float>(in.axis_x) / 32767.0f * pan_speed * dt;
        cam_target_.y += static_cast<float>(in.axis_y) / 32767.0f * pan_speed * dt;
        if (in.wheel != 0) {
            // Zoom around the cursor: the world point under it stays put.
            const Vec2 mouse{static_cast<float>(in.mouse_x), static_cast<float>(in.mouse_y)};
            zoom_anchor_ = mouse;
            const Vec2 before = view(cam_target_, zoom_target_).to_world(mouse);
            zoom_target_ = clampf(zoom_target_ * std::pow(1.25f, static_cast<float>(in.wheel)), 0.6f, 28.0f);
            const Vec2 after = view(cam_target_, zoom_target_).to_world(mouse);
            cam_target_ = cam_target_ + (before - after);
        }
        if (in.down(Button::MouseRight) && prev_input_.down(Button::MouseRight)) {
            cam_target_.x -= static_cast<float>(in.mouse_x - prev_input_.mouse_x) / zoom_target_;
            cam_target_.y -= static_cast<float>(in.mouse_y - prev_input_.mouse_y) / zoom_target_;
        }
        // Plain arithmetic smoothing on the fixed step: the view is part of
        // the simulation, so rewinding gives back the camera motion too.
        cam_center_ = cam_center_ + (cam_target_ - cam_center_) * clampf(16.0f * dt, 0, 1);
        zoom_ = std::exp(std::log(zoom_) + (std::log(zoom_target_) - std::log(zoom_)) * clampf(24.0f * dt, 0, 1));
    }

    // ---- commands and history ---------------------------------------------------

    void handle_command(const InputFrame& in) {
        const u32 value = static_cast<u32>(in.command_value);
        switch (in.command) {
        case kCmdTool:
            if (value < kToolCount) {
                tool_ = value;
                stop_drawing();
            }
            break;
        case kCmdShape:
            if (value <= static_cast<u32>(Shape::Curve)) shape_ = value;
            break;
        case kCmdType:
            if (value < road_types().size()) type_ = value;
            break;
        case kCmdGrid: snap_grid_ = !snap_grid_; break;
        case kCmdAngle: snap_angle_ = !snap_angle_; break;
        case kCmdLanes: show_lanes_ = !show_lanes_; break;
        case kCmdCars: cars_on_ = !cars_on_; break;
        case kCmdResize: {
            const float w = static_cast<float>(value & 0xFFFF), h = static_cast<float>(value >> 16);
            if (w < 320 || h < 200) break;
            view_w_ = w;
            view_h_ = h;
            zoom_ = zoom_target_; // nothing is mid-zoom about a point that has just moved
            zoom_anchor_ = {w * 0.5f, h * 0.5f};
            break;
        }
        case kCmdClockRate: clock_rate_ = (clock_rate_ + 1) % 3; break;
        case kCmdLightPreset:
            if (!can_have_lights(net_, sel_node_)) break;
            remember();
            if (in.command_value == kLightsOff) remove_lights(signals_, sel_node_);
            else if (value < kCustom) add_lights(net_, signals_, sel_node_, value);
            sel_program_ = sel_phase_ = 0;
            net_.invalidate(); // a crossing that comes or goes moves the stop lines
            refresh(net_);
            break;
        case kCmdWalk:
            if (SignalBox* box = signals_.at(sel_node_)) {
                remember();
                set_walk(*box, !box->walk);
                sel_phase_ = 0;
                net_.invalidate();
                refresh(net_);
            }
            break;
        case kCmdAdaptive:
            if (SignalBox* box = signals_.at(sel_node_))
                if (Program* p = program_of(*box, sel_program_)) p->adaptive = !p->adaptive;
            break;
        case kCmdProgram:
            if (SignalBox* box = signals_.at(sel_node_))
                if (value < box->programs.size()) {
                    sel_program_ = value;
                    sel_phase_ = 0;
                }
            break;
        case kCmdProgramAdd:
            if (SignalBox* box = signals_.at(sel_node_)) {
                remember();
                sel_program_ = add_program(*box, sel_program_);
                sel_phase_ = 0;
            }
            break;
        case kCmdProgramRemove:
            if (SignalBox* box = signals_.at(sel_node_)) {
                remember();
                remove_program(*box, sel_program_);
                sel_program_ = sel_phase_ = 0;
            }
            break;
        case kCmdStart:
            if (SignalBox* box = signals_.at(sel_node_)) shift_start(*box, sel_program_, in.command_value);
            break;
        case kCmdPhase:
            if (SignalBox* box = signals_.at(sel_node_))
                if (phase_of(*box, sel_program_, value)) sel_phase_ = value;
            break;
        case kCmdPhaseAdd:
            if (SignalBox* box = signals_.at(sel_node_)) {
                remember();
                sel_phase_ = add_phase(*box, sel_program_, sel_phase_);
            }
            break;
        case kCmdPhaseRemove:
            if (SignalBox* box = signals_.at(sel_node_)) {
                remember();
                remove_phase(*box, sel_program_, sel_phase_);
                if (sel_phase_ > 0) --sel_phase_;
            }
            break;
        case kCmdGreen:
            if (SignalBox* box = signals_.at(sel_node_)) change_green(*box, sel_program_, sel_phase_, static_cast<float>(in.command_value));
            break;
        case kCmdMaxGreen:
            if (SignalBox* box = signals_.at(sel_node_)) change_max_green(*box, sel_program_, sel_phase_, static_cast<float>(in.command_value));
            break;
        case kCmdAllRed:
            if (SignalBox* box = signals_.at(sel_node_))
                if (Program* p = program_of(*box, sel_program_))
                    p->plan.all_red = clampf(p->plan.all_red + static_cast<float>(in.command_value), 0, 10);
            break;
        case kCmdLaneArrow:
        case kCmdLaneAuto:
        case kCmdLaneAdd:
        case kCmdLaneRemove:
        case kCmdLaneFlip:
        case kCmdLanePocket: edit_lane(in.command, value); break;
        case kCmdUndo: step_history(undo_, redo_); break;
        case kCmdRedo: step_history(redo_, undo_); break;
        case kCmdClear:
            remember();
            signals_.boxes.clear();
            net_.clear();
            stop_drawing();
            break;
        case kCmdDemo:
            // Two maps to look at, turn about: the four set-piece junctions,
            // and a gallery of awkward ones laid the way a player lays them.
            remember();
            signals_.boxes.clear();
            net_.clear();
            if (demo_page_ % 2 == 0) build_demo();
            else build_gallery();
            ++demo_page_;
            refresh(net_);
            light_up();
            stop_drawing();
            break;
        default: break;
        }
    }

    Snapshot snapshot() {
        Writer w;
        SaveVisitor save(w);
        net_.reflect(save);
        signals_.reflect(save);
        return Snapshot{w.bytes()};
    }

    void restore(const Snapshot& s) {
        Reader r(s.bytes);
        LoadVisitor load(r);
        net_.reflect(load); // rebuilds only what differs from what is there now
        signals_.reflect(load);
        net_.invalidate(); // the lights decide where some lanes stop
        net_.rebuild();
    }

    /// Call before any edit: what is there now becomes a step that can be undone.
    void remember() {
        undo_.push_back(snapshot());
        if (undo_.size() > kUndoDepth) undo_.erase(undo_.begin());
        redo_.clear();
    }

    /// Undo and redo are the same move between two stacks.
    void step_history(std::vector<Snapshot>& from, std::vector<Snapshot>& to) {
        if (from.empty()) return;
        to.push_back(snapshot());
        const Snapshot back = from.back();
        from.pop_back();
        restore(back);
        stop_drawing();
    }

    void stop_drawing() {
        drawing_ = false;
        drag_node_ = drag_edge_ = kNone;
        hover_node_ = hover_edge_ = kNone;
        refusal_.clear();
        if (tool_ != kToolLights) sel_node_ = kNone;
        if (tool_ != kToolLanes) sel_lane_ = hover_lane_ = TrackRef{kNone, 0};
    }

    // ---- the build tool -----------------------------------------------------------

    float type_width() const { return road_types()[type_].width(); }

    /// Where the cursor ends up: stuck to the network if it is near enough,
    /// otherwise on an angle step or the grid if those are on.
    Snap find_snap(Vec2 world) const {
        Snap snap = snap_to_network(net_, world, px(kNodeReach), px(kEdgeReach));
        // Once stuck, it holds on a little: no flicker at the edge of its reach.
        if (!snap.on_network() && target_.on_network() && distance(world, target_.pos) < px(kNodeReach) * kHold) {
            if (target_.kind == Snap::Edge && net_.valid_edge(target_.edge)) {
                snap = target_;
                snap.pos = square_onto(net_, target_.edge, world);
            } else if (target_.kind != Snap::Edge && net_.valid_node(target_.node)) {
                snap = target_;
            }
        }
        if (drawing_ && snap.on_network()) {
            // Not back onto the place the road starts from.
            if ((snap.node != kNone && snap.node == start_.node) || distance(snap.pos, start_.pos) < 1.0f) {
                snap = Snap{};
                snap.pos = world;
            } else if (snap.kind == Snap::Edge && !(shape_ == static_cast<u32>(Shape::Curve) && start_.has_dir)) {
                // A straight road nearly square to the one it meets meets it square.
                const Vec2 foot = square_onto(net_, snap.edge, start_.pos);
                if (distance(foot, snap.pos) < px(26)) snap.pos = foot;
            }
        }
        if (snap.on_network()) return snap;

        if (drawing_ && snap_angle_) {
            // Steps of fifteen degrees, counted from the heading the road
            // starts with when it has one: straight on is always a step.
            const Vec2 v = world - start_.pos;
            const float base = start_.has_dir ? angle_of(start_.dir) : 0.0f;
            const float step = kAngleStep * 3.14159265f / 180.0f;
            const float angle = base + std::round((angle_of(v) - base) / step) * step;
            float reach = dot(v, from_angle(angle)); // the cursor's shadow on that ray
            if (snap_grid_) reach = std::round(reach / kGridStep) * kGridStep;
            snap.pos = start_.pos + from_angle(angle) * std::max(reach, 0.0f);
            snap.kind = Snap::Guide;
        } else if (snap_grid_) {
            snap.pos = {std::round(world.x / kGridStep) * kGridStep, std::round(world.y / kGridStep) * kGridStep};
            snap.kind = Snap::Grid;
        }
        return snap;
    }

    /// The road from where it starts to `end`, and whether it may be built.
    Plan plan_to(const Snap& end) const {
        Plan plan = plan_road(start_, end, static_cast<Shape>(shape_), type_width());
        if (plan.ok) {
            const char* why = road_problem(net_, plan.path, type_width());
            if (*why) {
                plan.ok = false;
                plan.problem = why;
            }
        }
        return plan;
    }

    /// A curve from open ground has no heading yet: the next click gives it one.
    bool needs_heading(const Snap& end) const {
        return shape_ == static_cast<u32>(Shape::Curve) && !start_.has_dir && !end.has_dir;
    }

    void tick_build(Vec2 world, bool pressed, bool cancel, bool on_panel, float dt) {
        hover_node_ = hover_edge_ = kNone;
        const Snap found = find_snap(world);
        const bool changed = found.kind != target_.kind || found.node != target_.node || found.edge != target_.edge;
        if (changed && found.on_network()) pulse_ = 0; // the ring snaps shut when something new is caught
        target_ = found;
        // The magnetic feel: stuck to something, the end of the road is pulled
        // across to it rather than jumping; free, it is simply where the cursor is.
        const Vec2 pull = target_.pos - shown_end_;
        shown_end_ = target_.kind == Snap::Free ? target_.pos : shown_end_ + pull * clampf(26.0f * dt, 0, 1);

        if (cancel) {
            drawing_ = false;
            refusal_.clear();
        }
        if (!pressed || on_panel) return;
        if (!drawing_) {
            start_ = target_;
            drawing_ = true;
            refusal_.clear();
            return;
        }
        if (needs_heading(target_)) {
            if (distance(target_.pos, start_.pos) > 2.0f) {
                start_.has_dir = true;
                start_.dir = normalize(target_.pos - start_.pos);
            }
            return;
        }
        const Plan plan = plan_to(target_);
        if (!plan.ok) {
            refusal_ = plan.problem;
            return;
        }
        remember();
        const BuildResult built = lay_road(net_, plan.path, type_);
        if (!built.ok) {
            undo_.pop_back();
            refusal_ = built.refusal;
            return;
        }
        refresh(net_);
        refusal_.clear();
        // Ended on something: done. Ended in the open: the next piece starts
        // here and already knows which way it is heading.
        if (target_.on_network()) {
            drawing_ = false;
            return;
        }
        Snap next;
        next.pos = plan.path.end();
        next.has_dir = true;
        next.dir = plan.path.segments.back().direction(1);
        next.node = net_.nearest_node(next.pos, 0.5f);
        start_ = next;
    }

    // ---- the other tools ------------------------------------------------------------

    /// What is under the cursor: a junction or road end first, else a road.
    void find_hover(Vec2 world, bool nodes) {
        hover_node_ = nodes ? net_.nearest_node(world, px(kNodeReach * 0.8f)) : kNone;
        hover_edge_ = hover_node_ == kNone ? net_.nearest_edge(world, px(kEdgeReach) + type_width() * 0.25f) : kNone;
    }

    void tick_move(Vec2 world, bool pressed, bool down, bool released, bool on_panel) {
        const Vec2 to = snap_grid_ ? Vec2{std::round(world.x / kGridStep) * kGridStep, std::round(world.y / kGridStep) * kGridStep} : world;
        if (drag_node_ != kNone || drag_edge_ != kNone) {
            if (down) {
                if (drag_node_ != kNone && net_.valid_node(drag_node_)) net_.move_node(drag_node_, to);
                else if (net_.valid_edge(drag_edge_)) bend_through(net_, drag_edge_, world);
                net_.rebuild(); // only the roads that moved are worked out again
            }
            if (released || !down) {
                // What it was dragged into has to be something that may exist:
                // if not, it goes back to where it was.
                const char* why = "";
                std::vector<u32> touched;
                if (net_.valid_node(drag_node_)) {
                    touched.push_back(drag_node_);
                    for (u32 e : net_.edges_at(drag_node_)) touched.push_back(net_.edge(e).a == drag_node_ ? net_.edge(e).b : net_.edge(e).a);
                } else if (net_.valid_edge(drag_edge_)) {
                    touched = {net_.edge(drag_edge_).a, net_.edge(drag_edge_).b};
                }
                for (u32 n : touched)
                    if (!*why) why = node_problem(net_, n);
                if (*why && !undo_.empty()) {
                    restore(undo_.back());
                    undo_.pop_back();
                    refusal_ = why;
                } else {
                    refresh(net_);
                    refusal_.clear();
                }
                drag_node_ = drag_edge_ = kNone;
            }
            return;
        }
        find_hover(world, true);
        if (!pressed || on_panel) return;
        if (hover_node_ == kNone && hover_edge_ == kNone) return;
        remember();
        drag_node_ = hover_node_;
        drag_edge_ = hover_edge_;
    }

    void tick_pick(Vec2 world, bool pressed, bool on_panel) {
        find_hover(world, false);
        if (!pressed || on_panel || hover_edge_ == kNone) return;
        remember();
        if (tool_ == kToolRemove) net_.remove_edge(hover_edge_);
        else apply_type(net_, hover_edge_, type_);
        refresh(net_);
        hover_edge_ = kNone;
    }

    // ---- lanes -----------------------------------------------------------------------------

    /// The lanes tool: click a lane to work on it.
    void tick_lanes(Vec2 world, bool pressed) {
        if (!lane_ok(sel_lane_)) sel_lane_ = TrackRef{kNone, 0};
        hover_node_ = hover_edge_ = kNone;
        Location here;
        hover_lane_ = net_.locate(world, here, 2.2f) ? here.track : TrackRef{kNone, 0};
        if (pressed) sel_lane_ = hover_lane_;
    }

    /// What the buttons of the lane editor do to the selected lane and its road.
    void edit_lane(u32 command, u32 value) {
        if (!lane_ok(sel_lane_)) return;
        const u32 e = sel_lane_.edge;
        const bool forward = net_.edge(e).tracks[sel_lane_.track].forward;
        remember();
        bool done = true;
        switch (command) {
        case kCmdLaneArrow: {
            // Start from what the lane does now, so the first click changes one thing.
            Track& lane = net_.edge(e).tracks[sel_lane_.track];
            const u32 now = lane.tag != kAny ? lane.tag : arrows_of(net_, sel_lane_);
            lane.tag = (now ^ value) & (kLeft | kRight | kStraight); // none left: back to the rules
            break;
        }
        case kCmdLaneAuto: net_.edge(e).tracks[sel_lane_.track].tag = kAny; break;
        case kCmdLaneAdd: done = add_lane(net_, e, forward); break;
        case kCmdLaneRemove:
            done = remove_lane(net_, e, forward);
            if (done) sel_lane_ = TrackRef{kNone, 0};
            break;
        case kCmdLaneFlip: {
            // The lane runs the other way: a third lane for the morning, a road made one-way.
            Track& lane = net_.edge(e).tracks[sel_lane_.track];
            done = lane.whole();
            if (done) {
                lane.forward = !lane.forward;
                lane.tag = kAny;
                // Pockets open out of a direction's kerb lane: with the lanes moved about they go.
                std::erase_if(net_.edge(e).tracks, [](const Track& t) { return !t.whole(); });
                sel_lane_ = TrackRef{kNone, 0};
            }
            break;
        }
        case kCmdLanePocket: done = remove_pocket(net_, e, forward) || add_pocket(net_, e, forward); break;
        default: done = false; break;
        }
        if (!done) {
            undo_.pop_back(); // nothing happened: nothing to undo
            return;
        }
        net_.invalidate();
        refresh(net_);
        if (!lane_ok(sel_lane_)) sel_lane_ = TrackRef{kNone, 0};
    }

    // ---- traffic lights --------------------------------------------------------------------

    /// What the paint has to know about the lights: where they are, and where people cross.
    u64 lights_key() const {
        u64 key = signals_.boxes.size();
        for (const SignalBox& box : signals_.boxes) key = key * 1099511628211ull + box.node * 2 + (box.walk ? 1 : 0);
        return key;
    }

    /// Every lane of a movement through the selected junction, as lines.
    std::vector<std::vector<Vec2>> lanes_of_move(u32 node, const Move& m) const {
        std::vector<std::vector<Vec2>> out;
        for (u32 c : net_.connectors_at(node)) {
            const Connector& k = net_.connectors()[c];
            if (k.from.edge != m.from || k.to.edge != m.to) continue;
            out.emplace_back();
            k.path.flatten(0.1f, out.back());
        }
        return out;
    }

    static float distance_to(const std::vector<Vec2>& line, Vec2 p) {
        float best = 1e30f;
        for (usize i = 0; i + 1 < line.size(); ++i) {
            const Vec2 ab = line[i + 1] - line[i];
            const float t = clampf(dot(p - line[i], ab) / std::max(length_sq(ab), 1e-6f), 0, 1);
            best = std::min(best, distance(p, line[i] + ab * t));
        }
        return best;
    }

    /// The movement under the cursor at the selected junction, as its group; 0 for none.
    u32 move_under(Vec2 world) const {
        const SignalBox* box = signals_.at(sel_node_);
        if (!box) return 0;
        u32 best = 0;
        float least = px(9.0f);
        for (u32 i = 0; i < box->moves.size(); ++i)
            for (const auto& lane : lanes_of_move(box->node, box->moves[i])) {
                const float d = distance_to(lane, world);
                if (d < least) {
                    least = d;
                    best = i + 1;
                }
            }
        return best;
    }

    /// The lights tool: click a junction to work on it; click one of its
    /// movements to put it in the selected phase or take it out.
    void tick_lights(Vec2 world, bool pressed) {
        if (!net_.valid_node(sel_node_) || !can_have_lights(net_, sel_node_)) sel_node_ = kNone;
        hover_edge_ = kNone;
        hover_node_ = net_.nearest_node(world, px(34.0f) + 8.0f);
        if (hover_node_ != kNone && !can_have_lights(net_, hover_node_)) hover_node_ = kNone;
        if (!pressed) return;
        if (const u32 group = move_under(world)) {
            SignalBox* box = signals_.at(sel_node_);
            if (phase_of(*box, sel_program_, sel_phase_)) {
                remember();
                toggle_move(*box, sel_program_, sel_phase_, group);
            }
            return;
        }
        if (hover_node_ != sel_node_) sel_program_ = sel_phase_ = 0;
        sel_node_ = hover_node_;
    }

    /// Are cars still arriving for a phase? An adaptive green waits for them.
    bool demand_for(const SignalBox& box, const SignalPhase& phase) const {
        for (u32 g : phase.groups) {
            if (g == 0 || g > box.moves.size()) continue;
            const u32 e = box.moves[g - 1].from;
            if (!net_.valid_edge(e)) continue;
            for (u32 t = 0; t < net_.edge(e).tracks.size(); ++t) {
                const TrackRef ref{e, t};
                if (!net_.arrives_at(ref, box.node)) continue;
                const float length = net_.track_length(ref);
                for (u32 a : cars_.on_track(net_.track_index(ref)))
                    if (length - cars_.agent(a).distance < 45.0f) return true;
            }
        }
        return false;
    }

    void tick_signals_and_clock(float dt) {
        // The roads changed: lights on a junction that is gone go with it, and
        // plans written for other roads are written again.
        if (signals_version_ != net_.version()) {
            const u64 before = lights_key();
            repair(net_, signals_);
            if (lights_key() != before) {
                net_.invalidate();
                refresh(net_);
            }
            signals_version_ = net_.version();
            if (const SignalBox* box = signals_.at(sel_node_)) {
                sel_program_ = std::min<u32>(sel_program_, static_cast<u32>(box->programs.size()) - 1);
                const usize phases = box->programs[sel_program_].plan.phases.size();
                sel_phase_ = phases ? std::min<u32>(sel_phase_, static_cast<u32>(phases) - 1) : 0;
            }
        }
        clock_ += dt * kClockRates[clock_rate_ % 3];
        if (clock_ >= 86400.0f) clock_ -= 86400.0f;
        tick_signals(signals_, dt, clock_ / 60.0f, [this](const SignalBox& box, const SignalPhase& phase) { return demand_for(box, phase); });
    }

    /// How busy the roads are at this time of day: twice as busy in the two
    /// rush hours as at midday, and nearly empty at night.
    float traffic_now() const {
        const float hour = clock_ / 3600.0f;
        const auto peak = [&](float at, float width) { return std::exp(-(hour - at) * (hour - at) / (2 * width * width)); };
        return clampf(0.25f + 0.75f * clampf((hour - 5.0f) / 2.0f, 0, 1) * clampf((23.0f - hour) / 3.0f, 0, 1) + peak(8.0f, 1.0f) + peak(17.0f, 1.2f), 0.2f, 2.2f);
    }

    // ---- traffic ------------------------------------------------------------------------

    /// Does this arm belong to a road with priority at the node? One that
    /// carries straight on does, unless a bigger road crosses it; one that
    /// ends there never does.
    bool carries_on(u32 node, u32 edge) const { return priority_at(net_, node).has(edge); }

    /// Is somebody with priority about to come through on a path that meets
    /// this connector's: crossing it, or arriving in the same lane?
    bool somebody_coming(const NetworkAgents& agents, u32 connector) const {
        const Connector& k = net_.connectors()[connector];
        const SignalBox* box = signals_.at(k.node);
        const int my_turn = turn_of(net_, k.from, k.to);
        const auto coming_on = [&](u32 other) {
            const Connector& o = net_.connectors()[other];
            if (box) {
                // At the lights: whoever has green and goes straight on, and
                // for a left turn the right turn coming the other way too.
                const int turn = turn_of(net_, o.from, o.to);
                if (aspect_of(*box, o.from.edge, o.to.edge) != Aspect::Green) return false;
                if (turn != 0 && !(turn == 1 && my_turn == -1)) return false;
            } else if (!priority_at(net_, o.node).along(net_, o)) {
                return false;
            }
            const float length = net_.track_length(o.from);
            for (u32 a : agents.on_track(net_.track_index(o.from))) {
                const NetAgent& car = agents.agent(a);
                // Close enough to be here within about three seconds.
                if (length - car.distance < 6.0f + car.speed * 3.0f) return true;
            }
            return false;
        };
        for (u32 index : net_.conflicts_of(connector)) {
            const Conflict& c = net_.conflicts()[index];
            if (coming_on(c.connector_a == connector ? c.connector_b : c.connector_a)) return true;
        }
        for (u32 other : net_.connectors_into(k.to))
            if (other != connector && coming_on(other)) return true;
        return false;
    }

    /// May a car enter a junction movement now? Never across somebody who is
    /// in the middle of theirs. A road that carries straight on has priority
    /// and otherwise just goes, except that turning left off it means waiting
    /// for what is coming the other way. A road that ends at the junction
    /// waits for a gap in the road it joins.
    bool may_go(const NetworkAgents& agents, u32, u32 connector) const {
        if (connector >= net_.connectors().size()) return true;
        const Connector& k = net_.connectors()[connector];
        for (u32 index : net_.conflicts_of(connector)) {
            const Conflict& c = net_.conflicts()[index];
            if (!agents.on_connector(c.connector_a == connector ? c.connector_b : c.connector_a).empty()) return false;
        }
        const int turn = turn_of(net_, k.from, k.to);
        // Lights first: red is red. Green means go, except that a turn across
        // somebody else's green gives way to them.
        if (const SignalBox* box = signals_.at(k.node)) {
            if (aspect_of(*box, k.from.edge, k.to.edge) != Aspect::Green) return false;
            return turn == 0 || !somebody_coming(agents, connector);
        }
        const Priority priority = priority_at(net_, k.node);
        if (priority.along(net_, k) || (priority.has(k.from.edge) && turn != -1)) return true;
        return !somebody_coming(agents, connector);
    }

    /// Nobody takes a corner at the speed of the open road: slow for a turn,
    /// and start slowing far enough back to be down to it on arrival.
    MotionLimits ease_for_turns(const NetworkAgents& agents, const CurveNetwork& net, u32 agent, MotionLimits m) const {
        const NetAgent& a = agents.agent(agent);
        const auto turning = [&](usize step) {
            if (step >= a.route.steps.size() || a.route.steps[step].kind != RouteStep::Kind::Connector) return false;
            const u32 c = a.route.steps[step].index;
            return c < net.connectors().size() && turn_of(net, net.connectors()[c].from, net.connectors()[c].to) != 0;
        };
        if (turning(a.step)) m.max_speed = std::min(m.max_speed, kTurnSpeed);
        else if (a.step < a.route.steps.size() && a.route.steps[a.step].kind == RouteStep::Kind::Track && turning(a.step + 1)) {
            const float left = net.track_length(net.track_ref(a.route.steps[a.step].index)) - a.distance;
            m.max_speed = std::min(m.max_speed, std::sqrt(kTurnSpeed * kTurnSpeed + 2.0f * 1.6f * std::max(left, 0.0f)));
        }
        return m;
    }

    static NetAgentParams driver(float wanted_speed) {
        NetAgentParams p;
        p.model = FollowModel::Idm;
        p.length = kCarLength;
        p.max_speed = wanted_speed;
        p.accel = 1.6f;
        p.decel = 2.6f;
        p.min_gap = 2.0f;
        p.headway = 1.2f;
        p.change_length = 22;
        return p;
    }

    /// Send one car from an open road end to another, if there is room to set off.
    void spawn_car() {
        if (cars_.count() >= kMostCars) return;
        std::vector<TrackRef> sources, sinks;
        for (u32 i = 0; i < net_.track_count(); ++i) {
            const TrackRef ref = net_.track_ref(i);
            const NetEdge& e = net_.edge(ref.edge);
            const Track& t = e.tracks[ref.track];
            if (turn_only(t.tag)) continue;
            const u32 from = t.forward ? e.a : e.b, to = t.forward ? e.b : e.a;
            if (net_.edges_at(from).size() == 1 && net_.leaves_from(ref, from)) sources.push_back(ref);
            if (net_.edges_at(to).size() == 1 && net_.arrives_at(ref, to)) sinks.push_back(ref);
        }
        if (sources.empty() || sinks.size() < 2) return;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const TrackRef from = sources[rng_.below(static_cast<u32>(sources.size()))];
            const TrackRef to = sinks[rng_.below(static_cast<u32>(sinks.size()))];
            if (from.edge == to.edge) continue;
            bool room = true;
            for (u32 a : cars_.on_track(net_.track_index(from))) room = room && cars_.agent(a).distance > 16.0f;
            if (!room) continue;
            const Route route = net_.route(from, to);
            if (!route.found) continue;
            const u32 id = cars_.spawn(route, kCarLength + 0.5f, driver(rng_.range_f(11.0f, 15.5f)), rng_.below(8)); // all of it on the road
            cars_.agent(id).speed = 8;
            if (car_info_.size() <= id) car_info_.resize(id + 1);
            car_info_[id].dest = net_.track_path(to).end();
            car_info_[id].pos = net_.track_path(from).start();
            return;
        }
    }

    /// The network changed under the cars: every route is in terms of a
    /// network that no longer exists. Each car is found again where it stands
    /// and given a new way to where it was going; one whose road is gone, goes.
    void replace_cars() {
        struct Kept {
            Route route;
            float distance;
            NetAgentParams params;
            u32 tag;
            float speed;
            Car info;
        };
        std::vector<Kept> kept;
        for (u32 i = 0; i < cars_.agents().size(); ++i) {
            if (!cars_.valid(i) || i >= car_info_.size()) continue;
            const Car& info = car_info_[i];
            Location here, there;
            if (!net_.locate(info.pos, here, 6.0f) || !net_.locate(info.dest, there, 8.0f)) continue;
            if (dot(net_.point_at(here).direction, info.dir) < 0.3f) continue; // that lane runs the other way
            const Route route = net_.route(here.track, there.track);
            if (!route.found) continue;
            kept.push_back(Kept{route, here.distance, cars_.agent(i).params, cars_.agent(i).tag, cars_.agent(i).speed, info});
        }
        cars_.clear();
        car_info_.clear();
        for (const Kept& k : kept) {
            const u32 id = cars_.spawn(k.route, k.distance, k.params, k.tag);
            cars_.agent(id).speed = k.speed;
            if (car_info_.size() <= id) car_info_.resize(id + 1);
            car_info_[id] = k.info;
        }
    }

    void tick_cars(float dt) {
        if (!cars_on_) {
            if (!cars_.agents().empty()) {
                cars_.clear();
                car_info_.clear();
            }
            cars_version_ = net_.version();
            return;
        }
        if (cars_version_ != net_.version()) {
            replace_cars();
            cars_version_ = net_.version();
        }
        spawn_timer_ += dt * traffic_now();
        while (spawn_timer_ >= kSpawnEvery) {
            spawn_timer_ -= kSpawnEvery;
            spawn_car();
        }
        cars_.tick(net_, dt);
        car_info_.resize(cars_.agents().size());
        for (u32 i = 0; i < cars_.agents().size(); ++i) {
            if (!cars_.valid(i)) continue;
            if (cars_.agent(i).done) {
                cars_.remove(i);
                continue;
            }
            const PathPoint at = cars_.pose(net_, i);
            car_info_[i].pos = at.position;
            if (length_sq(at.direction) > 0.5f) car_info_[i].dir = at.direction;
        }
    }

    void draw_cars(Renderer2D& r) {
        if (cars_version_ != net_.version()) return; // between an edit and the next tick: not where they were
        for (u32 i = 0; i < cars_.agents().size() && i < car_info_.size(); ++i) {
            if (!cars_.valid(i)) continue;
            const Car& car = car_info_[i];
            const Vec2 front = car.pos, back = car.pos - car.dir * kCarLength, side = perp(car.dir) * (kCarWidth * 0.5f);
            const Vec2 nose = car.dir * 0.35f; // a little off each front corner, so it has a front
            r.fill_polygon({back - side, front - nose - side, front - side * 0.6f, front + side * 0.6f, front - nose + side, back + side},
                           kCarColors[cars_.agent(i).tag % 8]);
            // A windscreen, and brake lights when it is braking.
            const Vec2 screen = car.pos - car.dir * 1.5f;
            r.fill_polygon({screen - side * 0.75f, screen + side * 0.75f, screen - car.dir * 0.9f + side * 0.75f, screen - car.dir * 0.9f - side * 0.75f},
                           rgba(30, 36, 46, 210));
            if (cars_.agent(i).accel < -0.7f) {
                for (float sign : {-1.0f, 1.0f}) {
                    const Vec2 at = back + side * (0.62f * sign);
                    r.fill_circle(at.x, at.y, 0.28f, rgb(255, 60, 48), 8);
                }
            }
        }
    }

    // ---- a few junctions to look at ---------------------------------------------------

    u32 road(u32 a, u32 b, std::vector<Track> lanes, const Path* path = nullptr) {
        const u32 e = net_.add_edge(a, b, path);
        if (e == kNone) return e;
        NetEdge& edge = net_.edge(e);
        float lo = 0, hi = 0;
        for (const Track& t : lanes) {
            lo = std::min(lo, t.offset - t.width * 0.5f);
            hi = std::max(hi, t.offset + t.width * 0.5f);
        }
        edge.tracks = std::move(lanes);
        edge.width = std::max(hi, -lo) * 2;
        return e;
    }
    u32 street(u32 a, u32 b) { return road(a, b, road_types()[0].lanes); }
    u32 avenue(u32 a, u32 b) { return road(a, b, road_types()[1].lanes); }

    /// The four junctions of the old junction lab, as a starting map: an
    /// angled T with a turn pocket, a five-way, a roundabout, and a rural T
    /// with a left-turn lane, an island and a slip lane.
    /// Lights the example maps start with, put on once the junctions exist.
    void light_up() {
        for (const auto& [at, preset] : demo_lights_) add_lights(net_, signals_, net_.nearest_node(at, 30), preset);
        demo_lights_.clear();
    }

    void build_demo() {
        demo_lights_ = {{{-110, -70}, kOpposite}, {{110, -70}, kEachRoad}};
        const auto polar = [](float degrees, float length) {
            const float a = degrees * 3.14159265f / 180;
            return Vec2{std::cos(a) * length, std::sin(a) * length};
        };
        {   // A side road joining a four-lane road at an angle.
            const Vec2 o{-110, -70};
            const u32 c = net_.add_node(o), w = net_.add_node(o + Vec2{-90, 0}), e = net_.add_node(o + Vec2{90, 0});
            const u32 s = net_.add_node(o + polar(128, 70));
            Track pocket = make_lane(2.5f, true, kRightOnly);
            pocket.begin = 32;
            pocket.taper = 18;
            std::vector<Track> lanes = road_types()[1].lanes;
            lanes.push_back(pocket);
            road(w, c, lanes);
            avenue(c, e);
            street(s, c);
        }
        {   // Five roads meeting at odd angles.
            const Vec2 o{110, -70};
            const u32 c = net_.add_node(o);
            const float angles[5] = {8, 76, 150, 214, 292};
            for (int i = 0; i < 5; ++i) {
                const u32 n = net_.add_node(o + polar(angles[i], i == 0 ? 85.0f : 65.0f));
                if (i == 0) avenue(n, c);
                else street(n, c);
            }
        }
        {   // A roundabout: four one-way arcs, traffic keeping right.
            const Vec2 o{-110, 70};
            constexpr float kRadius = 20;
            u32 ring[4];
            for (int i = 0; i < 4; ++i) ring[i] = net_.add_node(o + polar(static_cast<float>(i) * -90, kRadius));
            for (int i = 0; i < 4; ++i) {
                Path arc;
                arc.move_to(net_.node(ring[i]).pos);
                arc.arc_to(o, -3.14159265f * 0.5f);
                road(ring[i], ring[(i + 1) % 4], {make_lane(0, true, kAny, 5.0f)}, &arc);
                street(net_.add_node(o + polar(static_cast<float>(i) * -90, 62)), ring[i]);
            }
        }
        // A rural T as it is built: the main road widened on its north side
        // for a left-turn lane in the middle, and the right turn out of the
        // side road taken round an island, from a pocket to slow down in to
        // a lane on the main road to get up to speed in.
        const Vec2 o{95, 70};
        const auto at = [&](float x, float y) { return net_.add_node(o + Vec2{x, y}); };
        const u32 w = at(-120, 0), a = at(-62, 0), c = at(0, 0), x = at(42, 0), b = at(104, 0), e = at(150, 0);
        const u32 m = at(0, 42), s = at(0, 100);
        street(w, a);
        road(a, c, {make_lane(-1.5f, false), make_lane(0.5f, true)});
        road(c, x, {make_lane(-1.5f, false), make_lane(-0.5f, false, kLeftOnly), make_lane(0.5f, true)});
        Track turn_lane = make_lane(-0.5f, false, kLeftOnly); // runs against the edge: it begins at its far end
        turn_lane.end = 62 - 24;
        turn_lane.taper = 14;
        Track speed_up = make_lane(1.5f, true, kRightOnly);
        speed_up.end = 62 - 36;
        speed_up.taper = 22;
        road(x, b, {make_lane(-1.5f, false), turn_lane, make_lane(0.5f, true), speed_up});
        street(b, e);
        Track slow_down = make_lane(1.5f, true, kRightOnly);
        slow_down.begin = 20;
        slow_down.taper = 16;
        road(s, m, {make_lane(-0.5f, false), make_lane(0.5f, true), slow_down});
        const u32 side = street(m, c);
        Path slip;
        slip.move_to(net_.node(m).pos);
        slip.cubic_to(net_.node(m).pos + Vec2{0, -18}, net_.node(x).pos + Vec2{-22, 0}, net_.node(x).pos);
        road(m, x, {make_lane(1.5f, true, kRightOnly)}, &slip);

        // With a slip lane for it, the right turn at the junction itself is
        // closed: the kind of thing a player decides, so it is written into
        // the junction rather than derived by a rule.
        refresh(net_);
        for (u32 k : std::vector<u32>(net_.connectors_at(c))) {
            const Connector& turn = net_.connectors()[k];
            if (turn.from.edge == side && turn_of(net_, turn.from, turn.to) == 1) net_.allow_turn(c, turn.from, turn.to, false);
        }
    }

    /// Lay a road the way a click does, limits and all. False when it was refused.
    bool lay(u32 type, const Path& path) {
        if (*road_problem(net_, path, road_types()[type].width())) return false;
        const bool ok = lay_road(net_, path, type).ok;
        refresh(net_);
        return ok;
    }
    bool lay(u32 type, Vec2 a, Vec2 b) { return lay(type, straight(a, b)); }
    /// The place on the nearest road to `near`, for ending a road on it.
    Vec2 onto(Vec2 near) const { return snap_to_network(net_, near, 2.0f, 40.0f).pos; }

    /// A gallery of junctions that are awkward for one reason or another,
    /// every one of them laid with the same calls a player's clicks make.
    void build_gallery() {
        enum { kStreet, kAvenue, kBoulevard, kOneWay, kOneWay2 };
        const auto polar = [](float degrees, float length) {
            const float a = degrees * 3.14159265f / 180;
            return Vec2{std::cos(a) * length, std::sin(a) * length};
        };
        const auto cell = [](int column, int row) { return Vec2{-270.0f + 180.0f * static_cast<float>(column), -150.0f + 150.0f * static_cast<float>(row)}; };
        Vec2 o;

        o = cell(0, 0); // a big road crossed by a small one: the big one has priority
        lay(kAvenue, o + Vec2{-75, 0}, o + Vec2{75, 0});
        lay(kStreet, o + Vec2{0, -55}, o + Vec2{0, 55});

        o = cell(1, 0); // two streets crossing on the skew: equals
        lay(kStreet, o + Vec2{-75, 0}, o + Vec2{75, 0});
        lay(kStreet, o + polar(235, 62), o + polar(55, 62));

        o = cell(2, 0); // a fork
        lay(kStreet, o + Vec2{-75, 0}, o);
        lay(kStreet, o, o + polar(-20, 78));
        lay(kStreet, o, o + polar(20, 78));

        o = cell(3, 0); // two big roads crossing
        lay(kAvenue, o + Vec2{-75, 0}, o + Vec2{75, 0});
        lay(kAvenue, o + Vec2{0, -55}, o + Vec2{0, 55});

        o = cell(0, 1); // two side roads a few metres apart, on opposite sides
        lay(kStreet, o + Vec2{-75, 0}, o + Vec2{75, 0});
        lay(kStreet, o + Vec2{-14, -55}, o + Vec2{-14, 0});
        lay(kStreet, o + Vec2{14, 55}, o + Vec2{14, 0});

        o = cell(1, 1); // side roads on the outside and the inside of a bend
        lay(kStreet, arc_from(o + Vec2{-72, 35}, {1, 0}, o + Vec2{55, -45}));
        lay(kStreet, o + Vec2{20, 60}, onto(o + Vec2{10, 22}));
        lay(kStreet, o + Vec2{-40, -55}, onto(o + Vec2{-25, 30}));

        o = cell(2, 1); // one-way roads meeting a two-way one
        lay(kStreet, o + Vec2{-75, 0}, o + Vec2{75, 0});
        lay(kOneWay, o + Vec2{-35, 55}, o + Vec2{-35, 0});
        lay(kOneWay2, o + Vec2{30, -55}, o + Vec2{30, 55});

        o = cell(3, 1); // a six-lane road with a small and a middling side road
        lay(kBoulevard, o + Vec2{-80, 0}, o + Vec2{80, 0});
        lay(kStreet, o + Vec2{-35, 60}, o + Vec2{-35, 0});
        lay(kAvenue, o + Vec2{35, -60}, o + Vec2{35, 0});

        o = cell(0, 2); // five roads into one junction
        for (float angle : {0.0f, 70.0f, 140.0f, 215.0f, 290.0f}) lay(kStreet, o, o + polar(angle, 62));

        o = cell(1, 2); // two road ends, offset, joined
        lay(kStreet, o + Vec2{-78, -16}, o + Vec2{-30, -16});
        lay(kStreet, o + Vec2{30, 16}, o + Vec2{78, 16});
        lay(kStreet, connect(o + Vec2{-30, -16}, {1, 0}, o + Vec2{30, 16}, {1, 0}));

        o = cell(2, 2); // a road that changes size, and one that comes round a corner
        lay(kStreet, o + Vec2{-78, -20}, o + Vec2{-10, -20});
        lay(kAvenue, o + Vec2{-10, -20}, o + Vec2{78, -20});
        lay(kStreet, o + Vec2{-70, 45}, o + Vec2{-10, 45});
        lay(kStreet, arc_from(o + Vec2{-10, 45}, {1, 0}, o + Vec2{20, 15}));
        lay(kStreet, onto(o + Vec2{20, 15}), onto(o + Vec2{20, -20}));

        o = cell(3, 2); // a big road that ends on a small one
        lay(kStreet, o + Vec2{-75, 20}, o + Vec2{75, 20});
        lay(kAvenue, o + Vec2{0, -55}, o + Vec2{0, 20});

        // The four places the marking rules ask for guide lines.
        o = cell(0, 3); // two lanes turning left side by side
        lay(kBoulevard, o + Vec2{-80, -20}, o + Vec2{80, -20});
        if (*road_problem(net_, straight(o + Vec2{0, 50}, o + Vec2{0, -20}), 7.0f) == 0) {
            const BuildResult both = lay_road(net_, straight(o + Vec2{0, 50}, o + Vec2{0, -20}), kOneWay2);
            if (both.ok && !both.edges.empty())
                for (Track& lane : net_.edge(both.edges.back()).tracks) lane.tag = kLeftOnly;
            net_.invalidate();
            refresh(net_);
        }

        o = cell(1, 3); // two big roads, one ending on the other
        lay(kAvenue, o + Vec2{0, -55}, o + Vec2{0, 55});
        lay(kAvenue, o + Vec2{-75, 0}, o + Vec2{0, 0});

        o = cell(2, 3); // the priority road turns the corner
        lay(kAvenue, o + Vec2{-75, 0}, o);
        lay(kAvenue, o, o + Vec2{0, 55});
        lay(kStreet, o, o + Vec2{70, 0});
        lay(kStreet, o, o + Vec2{0, -55});

        o = cell(3, 3); // a wide junction on the skew
        lay(kBoulevard, o + Vec2{-80, 0}, o + Vec2{80, 0});
        lay(kAvenue, o + polar(235, 62), o + polar(55, 62));

        // Traffic lights on three of them, one of each kind, to start from.
        add_lights(net_, signals_, net_.nearest_node(cell(3, 0), 10), kProtected);
        add_lights(net_, signals_, net_.nearest_node(cell(1, 0), 10), kOpposite);
        add_lights(net_, signals_, net_.nearest_node(cell(3, 3), 10), kEachRoad);
        demo_lights_ = {{cell(2, 1) + Vec2{30, 0}, kOpposite}, {cell(3, 1) + Vec2{35, 0}, kProtected}, {cell(0, 0), kProtected}};
    }

    // ---- drawing ------------------------------------------------------------------------

    void draw_grid(Renderer2D& r) {
        const Camera cam = view(view_center(), zoom_);
        const Vec2 lo = cam.to_world({0, 0}), hi = cam.to_world({view_w_, view_h_});
        // Ten metres between lines, fifty between the stronger ones; zoomed
        // far out, only the stronger ones.
        const float step = zoom_ < 1.6f ? 50.0f : 10.0f;
        const float thin = px(1.0f);
        for (float x = std::floor(lo.x / step) * step; x <= hi.x; x += step) {
            const bool major = std::fabs(std::remainder(x, 50.0f)) < 0.01f;
            r.fill_rect(x - thin * 0.5f, lo.y, thin, hi.y - lo.y, major ? kGridMajor : kGridMinor);
        }
        for (float y = std::floor(lo.y / step) * step; y <= hi.y; y += step) {
            const bool major = std::fabs(std::remainder(y, 50.0f)) < 0.01f;
            r.fill_rect(lo.x, y - thin * 0.5f, hi.x - lo.x, thin, major ? kGridMajor : kGridMinor);
        }
    }

    static StrokeStyle stroke(Color c, float width, float dash = 0, float gap = 0) {
        StrokeStyle s;
        s.width = width;
        s.dash = dash;
        s.gap = gap;
        s.join = LineJoin::Round;
        s.r = c.r / 255.0f;
        s.g = c.g / 255.0f;
        s.b = c.b / 255.0f;
        s.a = c.a / 255.0f;
        return s;
    }

    void ring(Renderer2D& r, Vec2 at, float radius, Color c, float width) {
        std::vector<Vec2> points;
        for (int i = 0; i < 32; ++i) points.push_back(at + from_angle(6.2831853f * static_cast<float>(i) / 32) * radius);
        StrokeStyle s = stroke(c, width);
        s.closed = true;
        r.draw_polyline(points, s);
    }

    /// A road drawn over the top of everything, see-through: one about to be
    /// built, or one the cursor is on.
    void ghost(Renderer2D& r, const Path& path, float width, Color fill_color, bool lane_lines) {
        std::vector<Vec2> line;
        path.flatten(0.05f, line);
        if (line.size() < 2) return;
        BandOptions cap;
        cap.cap = PolyCap::Round;
        Mesh mesh;
        RoadPaint::fill(polyline_band(line, width, cap), fill_color, mesh, fill_color.a / 255.0f);
        r.draw_mesh(mesh);
        if (!lane_lines) return;
        // Its lanes, so it reads as the road it will be.
        for (const Track& t : road_types()[type_].lanes) {
            const float edge = t.offset - t.width * 0.5f;
            if (std::fabs(edge + width * 0.5f) < 0.05f) continue; // the kerb itself
            r.draw_polyline(offset_polyline(line, edge, false), stroke(kGhostLine, px(1.2f), 2.0f, 2.5f));
        }
    }

    void label(Renderer2D& r, Vec2 world, float dx, float dy, const char* text, Color c, int scale = 2) {
        // Text is in screen pixels whatever the camera: scaled by hand.
        const Vec2 at = view(view_center(), zoom_).to_screen(world);
        const float s = ui_scale();
        const int size = scale * static_cast<int>(s);
        r.draw_text(at.x + (dx + 1) * s, at.y + (dy + 1) * s, text, rgba(0, 0, 0, 150), size);
        r.draw_text(at.x + dx * s, at.y + dy * s, text, c, size);
    }

    /// What the cursor has caught: a ring that closes on it.
    void draw_magnet(Renderer2D& r, const Snap& snap) {
        const float t = clampf(pulse_ * 5.0f, 0, 1);
        const float settle = 1.0f + (1.0f - t) * (1.0f - t) * 1.6f; // starts wide, closes in
        switch (snap.kind) {
        case Snap::End:
        case Snap::Node:
            ring(r, snap.pos, px(11) * settle, kMagnet, px(2.5f));
            r.fill_circle(snap.pos.x, snap.pos.y, px(3.5f), kMagnet);
            break;
        case Snap::Edge: {
            ring(r, snap.pos, px(8) * settle, kMagnet, px(2));
            // The road it is on, lit up either side of the place.
            if (net_.valid_edge(snap.edge)) {
                const PathSampler sampler(net_.edge(snap.edge).path, 0.1f);
                const float here = sampler.closest(snap.pos).distance;
                const Path stretch = net_.edge(snap.edge).path.sub_path(here - 14, here + 14, 0.05f);
                if (!stretch.empty()) ghost(r, stretch, net_.edge(snap.edge).width, rgba(255, 204, 92, 60), false);
            }
            break;
        }
        case Snap::Grid:
        case Snap::Guide: r.fill_circle(snap.pos.x, snap.pos.y, px(3), rgba(255, 255, 255, 200)); break;
        default: break;
        }
    }

    static Color lamp_color(Aspect a) {
        switch (a) {
        case Aspect::Green: return kLampGreen;
        case Aspect::Amber: return kLampAmber;
        case Aspect::RedAmber: return rgb(240, 130, 50);
        default: return kLampRed;
        }
    }

    /// A lamp at the end of every lane that arrives at the lights, showing
    /// the best any of its movements has: a lane that may go straight on at
    /// green shows green, though its left turn may still have to wait.
    void draw_lamps(Renderer2D& r) {
        for (const SignalBox& box : signals_.boxes) {
            if (!net_.valid_node(box.node)) continue;
            for (u32 e : net_.edges_at(box.node)) {
                for (u32 t = 0; t < net_.edge(e).tracks.size(); ++t) {
                    const TrackRef ref{e, t};
                    if (!net_.arrives_at(ref, box.node) || net_.track_path(ref).empty()) continue;
                    int best = -1;
                    Aspect shown = Aspect::Red;
                    for (u32 c : net_.connectors_from(ref)) {
                        const Connector& k = net_.connectors()[c];
                        if (k.node != box.node) continue;
                        const Aspect a = aspect_of(box, k.from.edge, k.to.edge);
                        const int rank = a == Aspect::Green ? 3 : a == Aspect::Amber ? 2 : a == Aspect::RedAmber ? 1 : 0;
                        if (rank > best) {
                            best = rank;
                            shown = a;
                        }
                    }
                    if (best < 0) continue;
                    const Path& path = net_.track_path(ref);
                    const Vec2 at = path.end() - path.segments.back().direction(1) * 1.9f;
                    const float lamp = std::max(0.72f, px(4.0f)); // never too small to read from far out
                    r.fill_circle(at.x, at.y, lamp * 1.45f, rgb(24, 26, 30), 20);
                    r.fill_circle(at.x, at.y, lamp, lamp_color(shown), 20);
                }
            }
            // People crossing: a white dot on each side of every road while they may.
            if (walking(box)) {
                for (u32 e : net_.edges_at(box.node))
                    for (u32 t = 0; t < net_.edge(e).tracks.size(); ++t) {
                        const TrackRef ref{e, t};
                        if (!net_.arrives_at(ref, box.node) || net_.track_path(ref).empty()) continue;
                        const Path& path = net_.track_path(ref);
                        const Vec2 at = path.end() + path.segments.back().direction(1) * 2.4f;
                        r.fill_circle(at.x, at.y, 0.55f, rgb(250, 250, 244), 16);
                    }
            }
        }
    }

    /// The lights tool on the map: the junction being worked on, and its
    /// movements as arrows: green where they are in the selected phase, amber
    /// where they have green but give way, red where two that go straight on
    /// cross each other.
    void draw_lights_tool(Renderer2D& r) {
        if (hover_node_ != kNone && hover_node_ != sel_node_ && net_.valid_node(hover_node_)) {
            const NetNode& n = net_.node(hover_node_);
            r.draw_polyline(ring(n.pos, n.radius + 4.0f), stroke(kMagnet, px(2.0f), 2.0f, 1.5f));
        }
        if (!net_.valid_node(sel_node_)) return;
        const NetNode& node = net_.node(sel_node_);
        r.draw_polyline(ring(node.pos, node.radius + 5.0f), stroke(kAccent, px(2.5f)));
        const SignalBox* box = signals_.at(sel_node_);
        if (!box || sel_program_ >= box->programs.size()) return;
        const SignalPlan& plan = box->programs[sel_program_].plan;
        if (sel_phase_ >= plan.phases.size()) return;
        const SignalPhase& phase = plan.phases[sel_phase_];
        const u32 under = move_under(cursor_);
        for (int pass = 0; pass < 2; ++pass)
            for (u32 i = 0; i < box->moves.size(); ++i) {
                const bool in = phase.has(i + 1);
                if (in != (pass == 1)) continue; // those in the phase on top
                Color c = rgba(236, 238, 244, 90);
                if (in) {
                    c = kLampGreen;
                    const int turn = turn_of_move(net_, box->node, box->moves[i]);
                    for (u32 g : phase.groups) {
                        if (g == i + 1 || g > box->moves.size() || !moves_cross(net_, box->node, box->moves[i], box->moves[g - 1])) continue;
                        const int other = turn_of_move(net_, box->node, box->moves[g - 1]);
                        if (turn == 0 && other == 0) c = kLampRed;
                        else if (turn != 0 && c.r == kLampGreen.r) c = kLampAmber;
                    }
                }
                const float width = px(i + 1 == under ? 4.5f : in ? 3.0f : 1.6f);
                for (const auto& lane : lanes_of_move(box->node, box->moves[i])) {
                    if (lane.size() < 2) continue;
                    r.draw_polyline(lane, stroke(c, width));
                    const Vec2 tip = lane.back(), dir = normalize(lane.back() - lane[lane.size() - 2]), side = perp(dir);
                    const float s = px(in ? 7.0f : 5.0f);
                    r.fill_polygon({tip + dir * s, tip - side * (s * 0.6f), tip + side * (s * 0.6f)}, c);
                }
            }
    }

    static std::vector<Vec2> ring(Vec2 at, float radius) {
        std::vector<Vec2> out;
        for (int i = 0; i <= 48; ++i) {
            const float a = static_cast<float>(i) / 48.0f * 6.2831853f;
            out.push_back(at + Vec2{std::cos(a), std::sin(a)} * radius);
        }
        return out;
    }

    /// A lane picked out: its two edges, and where it goes at the junction ahead.
    void draw_lane(Renderer2D& r, TrackRef lane, Color c, bool with_turns) {
        if (!lane_ok(lane) || net_.track_path(lane).empty()) return;
        for (float side : {-1.0f, 1.0f}) r.draw_polyline(track_edge(net_, lane, side, 0.2f), stroke(c, px(2.5f)));
        if (!with_turns) return;
        for (u32 k : net_.connectors_from(lane)) {
            std::vector<Vec2> path;
            net_.connectors()[k].path.flatten(0.1f, path);
            if (path.size() < 2) continue;
            r.draw_polyline(path, stroke(c, px(3.0f)));
            const Vec2 tip = path.back(), dir = normalize(path.back() - path[path.size() - 2]), side = perp(dir);
            r.fill_polygon({tip + dir * px(7.0f), tip - side * px(4.5f), tip + side * px(4.5f)}, c);
        }
    }

    void draw_tool(Renderer2D& r) {
        if (tool_ == kToolLights) {
            draw_lights_tool(r);
            return;
        }
        if (tool_ == kToolLanes) {
            if (!(hover_lane_ == sel_lane_)) draw_lane(r, hover_lane_, kMagnet, false);
            draw_lane(r, sel_lane_, kAccent, true);
            return;
        }
        char text[96];
        if (tool_ == kToolBuild) {
            Snap shown = target_;
            shown.pos = shown_end_;
            if (drawing_) {
                if (needs_heading(shown)) {
                    // A heading still to be set: a line to show which way.
                    r.draw_polyline({start_.pos, shown.pos}, stroke(kGhostLine, px(1.5f), 2.0f, 2.0f));
                    label(r, shown.pos, 16, 14, "click to set the heading", rgb(255, 240, 200), 1);
                } else {
                    const Plan plan = plan_to(shown);
                    if (!plan.path.empty()) {
                        ghost(r, plan.path, type_width(), plan.ok ? kGhost : kGhostBad, true);
                        // A ring wherever it crosses a road: a junction will form there.
                        for (const Crossing& c : find_crossings(net_, plan.path, 3.0f))
                            if (c.along > 4 && c.along < plan.length - 4) ring(r, c.at, px(9), rgb(255, 255, 255), px(2));
                        if (plan.radius > 0) std::snprintf(text, sizeof text, "%.0f m   R %.0f m", plan.length, plan.radius);
                        else std::snprintf(text, sizeof text, "%.0f m", plan.length);
                        label(r, shown.pos, 16, 14, text, plan.ok ? rgb(255, 240, 200) : rgb(255, 150, 140));
                        if (!plan.ok) label(r, shown.pos, 16, 34, plan.problem, rgb(255, 150, 140), 1);
                        else if (plan.joined) label(r, shown.pos, 16, 34, "joined", kMagnet, 1);
                    }
                }
                ring(r, start_.pos, px(6), rgb(255, 255, 255), px(2));
            }
            draw_magnet(r, shown);
            if (!refusal_.empty()) label(r, cursor_, 16, -22, refusal_.c_str(), rgb(255, 150, 140), 2);
            return;
        }
        if (!refusal_.empty()) label(r, cursor_, 16, -22, refusal_.c_str(), rgb(255, 150, 140), 2);
        // The other tools light up what a click would act on.
        const Color tint = tool_ == kToolRemove ? rgba(226, 78, 66, 130) : tool_ == kToolChange ? kGhost : rgba(255, 204, 92, 90);
        const u32 edge = drag_edge_ != kNone ? drag_edge_ : hover_edge_;
        const u32 node = drag_node_ != kNone ? drag_node_ : hover_node_;
        if (net_.valid_edge(edge)) ghost(r, net_.edge(edge).path, net_.edge(edge).width + 1.0f, tint, false);
        if (net_.valid_node(node)) {
            ring(r, net_.node(node).pos, px(12), kMagnet, px(2.5f));
            r.fill_circle(net_.node(node).pos.x, net_.node(node).pos.y, px(4), kMagnet);
        }
    }

    // ---- the panel ------------------------------------------------------------------------

    UiStyle skin() const {
        UiStyle s;
        s.panel = rgba(246, 246, 243, 244);
        s.panel_border = rgba(198, 198, 192, 255);
        s.title = rgb(38, 40, 46);
        s.text = rgb(46, 48, 54);
        s.text_dim = rgb(122, 124, 130);
        s.button = rgb(233, 233, 229);
        s.button_hover = rgb(220, 226, 236);
        s.button_active = kAccent;
        s.button_border = rgba(196, 198, 200, 255);
        s.accent = kAccent;
        s.track = rgb(214, 214, 208);
        s.tooltip = rgba(28, 30, 36, 244);
        s.rounding = 5;
        s.item_height = 24;
        return s;
    }

    void outline_if(Renderer2D& r, const Rect& box, bool on, Color c = kAccent) const {
        if (!on) return;
        r.draw_rect({box.x - 2, box.y - 2, box.w + 4, box.h + 4}, c);
        r.draw_rect({box.x - 1, box.y - 1, box.w + 2, box.h + 2}, c);
    }

    const char* hint() const {
        switch (tool_) {
        case kToolBuild:
            if (!drawing_) return "CLICK WHERE THE ROAD STARTS. IT STICKS TO ROAD ENDS, JUNCTIONS AND ROADS.";
            return shape_ == static_cast<u32>(Shape::Curve) ? "CLICK WHERE IT ENDS. A CURVE LEAVES ALONG ITS HEADING. RIGHT CLICK STOPS."
                                                            : "CLICK WHERE IT ENDS. ON ANOTHER ROAD, A JUNCTION FORMS. RIGHT CLICK STOPS.";
        case kToolMove: return "DRAG A JUNCTION OR A ROAD END TO MOVE IT, THE MIDDLE OF A ROAD TO BEND IT.";
        case kToolRemove: return "CLICK A ROAD TO TAKE IT OUT.";
        case kToolLanes:
            return lane_ok(sel_lane_) ? "ARROWS SAY WHICH WAY THIS LANE MAY GO AT THE JUNCTION AHEAD. THE REST CHANGES THE ROAD."
                                      : "CLICK A LANE TO GIVE IT ARROWS, OR TO ADD AND REMOVE LANES ON ITS ROAD.";
        case kToolLights:
            if (!net_.valid_node(sel_node_)) return "CLICK A JUNCTION TO GIVE IT TRAFFIC LIGHTS, OR TO CHANGE THE ONES IT HAS.";
            if (!signals_.at(sel_node_)) return "PICK HOW THE LIGHTS START OUT. EVERYTHING CAN BE CHANGED AFTERWARDS.";
            return "CLICK A MOVEMENT ON THE JUNCTION TO PUT IT IN THE SELECTED PHASE, OR TAKE IT OUT.";
        default: return "CLICK A ROAD TO MAKE IT THE SELECTED KIND.";
        }
    }

    /// Small lettering in a panel, in UI units.
    void note(Renderer2D& r, float x, float y, const char* text, Color c = rgb(96, 98, 104)) const {
        r.draw_text(x * ui_scale(), y * ui_scale(), text, c, static_cast<int>(ui_scale()));
    }

    /// The lane editor: the selected lane's arrows, and the lanes of its road.
    void draw_lanes_panel(Renderer2D& r) {
        const Rect box_rect = lanes_panel();
        ui_.begin_panel(box_rect);
        const float x = box_rect.x + 10, w = box_rect.w - 20;
        float y = box_rect.y + 10;
        char text[64];
        const NetEdge& edge = net_.edge(sel_lane_.edge);
        const Track& lane = edge.tracks[sel_lane_.track];
        const auto button = [&](float bx, float by, float bw, const char* label, u32 command, i32 value = 0, bool on = false) {
            const Rect b{bx, by, bw, 24};
            if (ui_.button_at(b, label)) app_->push_command(command, value);
            outline_if(r, b, on);
        };
        const u32 arrows = lane.tag != kAny ? lane.tag : arrows_of(net_, sel_lane_);
        note(r, x, y, lane.tag != kAny ? "THIS LANE GOES   (PAINTED ON IT)" : "THIS LANE GOES   (BY THE RULES)", rgb(46, 48, 54));
        y += 14;
        const float quarter = (w - 12) / 4;
        button(x, y, quarter, "Left", kCmdLaneArrow, kLeft, (arrows & kLeft) != 0);
        button(x + quarter + 4, y, quarter, "Ahead", kCmdLaneArrow, kStraight, (arrows & kStraight) != 0);
        button(x + (quarter + 4) * 2, y, quarter, "Right", kCmdLaneArrow, kRight, (arrows & kRight) != 0);
        button(x + (quarter + 4) * 3, y, quarter, "Auto", kCmdLaneAuto, 0, lane.tag == kAny);
        y += 36;

        std::snprintf(text, sizeof text, "THIS ROAD   %u LANE%s THIS WAY, %u THE OTHER", lanes_one_way(edge, lane.forward),
                      lanes_one_way(edge, lane.forward) == 1 ? "" : "S", lanes_one_way(edge, !lane.forward));
        note(r, x, y, text, rgb(46, 48, 54));
        y += 14;
        const float half = (w - 4) / 2;
        button(x, y, half, "Add lane", kCmdLaneAdd);
        button(x + half + 4, y, half, "Remove lane", kCmdLaneRemove);
        y += 28;
        bool pocket = false;
        for (const Track& t : edge.tracks) pocket = pocket || (t.forward == lane.forward && !t.whole());
        button(x, y, half, "Reverse", kCmdLaneFlip);
        button(x + half + 4, y, half, "Right pocket", kCmdLanePocket, 0, pocket);
        y += 36;
        const Rect bands{x, y, half, 24};
        if (ui_.button_at(bands, "Show bands")) app_->push_command(kCmdLanes);
        outline_if(r, bands, show_lanes_);
        y += 34;
        note(r, x, y, "ADD AND REMOVE WORK AT THE KERB OF THIS LANE'S SIDE.");
        note(r, x, y + 12, "REVERSE TURNS THIS ONE LANE THE OTHER WAY.");
        note(r, x, y + 24, "A POCKET IS A SHORT LANE FOR TURNING RIGHT.");
        ui_.end_panel();
    }

    /// The lights editor. Top to bottom: how the junction starts out, the
    /// programs and when each begins, the phases of the selected program,
    /// and the timing of the selected phase.
    void draw_lights_panel(Renderer2D& r) {
        const Rect box_rect = lights_panel();
        ui_.begin_panel(box_rect);
        const float x = box_rect.x + 10, w = box_rect.w - 20;
        float y = box_rect.y + 10;
        char text[64];
        const SignalBox* box = signals_.at(sel_node_);
        const auto button = [&](float bx, float by, float bw, const char* label, u32 command, i32 value = 0, bool on = false, float bh = 24) {
            const Rect b{bx, by, bw, bh};
            if (ui_.button_at(b, label)) app_->push_command(command, value);
            outline_if(r, b, on);
        };

        note(r, x, y, "TRAFFIC LIGHTS", rgb(46, 48, 54));
        y += 14;
        const float quarter = (w - 12) / 4;
        button(x, y, quarter, "Off", kCmdLightPreset, kLightsOff, !box);
        for (u32 i = 0; i < 3; ++i)
            button(x + (quarter + 4) * static_cast<float>(i + 1), y, quarter, kPresetNames[i], kCmdLightPreset, static_cast<i32>(i), box && box->preset == i);
        y += 30;
        if (!box) {
            note(r, x, y, "SIMPLE  EVERY ROAD HAS ITS OWN GREEN");
            note(r, x, y + 12, "FACING  ROADS ACROSS FROM EACH OTHER GO");
            note(r, x, y + 24, "        TOGETHER, LEFT TURNS WAIT FOR A GAP");
            note(r, x, y + 36, "LEFTS   LEFT TURNS GET A PHASE TO THEMSELVES");
            ui_.end_panel();
            return;
        }
        const Program& program = box->programs[std::min<usize>(sel_program_, box->programs.size() - 1)];
        const float half = (w - 4) / 2;
        button(x, y, half, "Crossing", kCmdWalk, 0, box->walk);
        button(x + half + 4, y, half, "Adaptive", kCmdAdaptive, 0, program.adaptive);
        y += 34;

        // Programs: one button each, named by the time it starts.
        std::snprintf(text, sizeof text, "PROGRAMS BY TIME OF DAY%s", box->preset == kCustom ? "     CUSTOM" : "");
        note(r, x, y, text, rgb(46, 48, 54));
        y += 14;
        for (u32 i = 0; i < box->programs.size(); ++i) {
            std::snprintf(text, sizeof text, "%02u:%02u", box->programs[i].start / 60, box->programs[i].start % 60);
            const float bx = x + (quarter + 4) * static_cast<float>(i);
            button(bx, y, quarter, text, kCmdProgram, static_cast<i32>(i), sel_program_ == i);
            if (box->running == i) r.fill_rect(bx + 4, y + 4, 5, 5, kLampGreen); // the one in force now
        }
        y += 28;
        const float sixth = (w - 10) / 6;
        const char* const shifts[4] = {"-1h", "-10", "+10", "+1h"};
        const i32 by_minutes[4] = {-60, -10, 10, 60};
        for (u32 i = 0; i < 4; ++i) button(x + (sixth + 2) * static_cast<float>(i), y, sixth, shifts[i], kCmdStart, by_minutes[i]);
        button(x + (sixth + 2) * 4, y, sixth, "New", kCmdProgramAdd);
        button(x + (sixth + 2) * 5, y, sixth, "Del", kCmdProgramRemove);
        y += 34;

        // Phases: the cycle, one row each.
        std::snprintf(text, sizeof text, "PHASES     CYCLE %.0f S", program.plan.cycle());
        note(r, x, y, text, rgb(46, 48, 54));
        y += 14;
        const float row = w - 72;
        for (u32 i = 0; i < program.plan.phases.size(); ++i) {
            const SignalPhase& phase = program.plan.phases[i];
            const bool walk = phase.groups.size() == 1 && phase.groups[0] == box->walk_group();
            if (walk) std::snprintf(text, sizeof text, "%u  %3.0f s  walk", i + 1, phase.green);
            else std::snprintf(text, sizeof text, "%u  %3.0f s  %zu moves", i + 1, phase.green, phase.groups.size());
            button(x, y, row, text, kCmdPhase, static_cast<i32>(i), sel_phase_ == i, 22);
            // Running now, and whether what runs together in it is safe.
            if (box->running == sel_program_ && box->state.phase == i) r.fill_rect(x + 4, y + 4, 5, 5, kLampGreen);
            const Clash clash = phase_clash(net_, *box, phase);
            if (clash != Clash::None) r.fill_rect(x + row - 10, y + 4, 5, 5, clash == Clash::Unsafe ? kLampRed : kLampAmber);
            if (i == 0) {
                button(x + row + 4, y, 32, "+", kCmdPhaseAdd, 0, false, 22);
                button(x + row + 40, y, 32, "-", kCmdPhaseRemove, 0, false, 22);
            }
            y += 25;
        }
        y += 8;

        // The selected phase: how long.
        if (sel_phase_ < program.plan.phases.size()) {
            const SignalPhase& phase = program.plan.phases[sel_phase_];
            std::snprintf(text, sizeof text, "GREEN %.0f S", phase.green);
            note(r, x, y + 8, text, rgb(46, 48, 54));
            const char* const steps[4] = {"-5", "-1", "+1", "+5"};
            const i32 by[4] = {-5, -1, 1, 5};
            for (u32 i = 0; i < 4; ++i) button(x + 100 + 56 * static_cast<float>(i), y, 52, steps[i], kCmdGreen, by[i]);
            y += 28;
            std::snprintf(text, sizeof text, "UP TO %.0f S", std::max(phase.max_green, phase.green));
            note(r, x, y + 8, text, program.adaptive ? rgb(46, 48, 54) : rgb(160, 162, 166));
            button(x + 100, y, 52, "-5", kCmdMaxGreen, -5);
            button(x + 268, y, 52, "+5", kCmdMaxGreen, 5);
            y += 28;
        }
        std::snprintf(text, sizeof text, "ALL RED %.0f S", program.plan.all_red);
        note(r, x, y + 8, text, rgb(46, 48, 54));
        button(x + 100, y, 52, "-1", kCmdAllRed, -1);
        button(x + 268, y, 52, "+1", kCmdAllRed, 1);
        y += 32;

        // What is wrong with the plan, if anything.
        bool unsafe = false;
        for (const SignalPhase& phase : program.plan.phases) unsafe = unsafe || phase_clash(net_, *box, phase) == Clash::Unsafe;
        const u32 left_out = moves_left_out(*box, program.plan);
        if (unsafe) note(r, x, y, "RED MARK: TWO ROADS CROSS EACH OTHER AT GREEN", kLampRed);
        else if (left_out) {
            std::snprintf(text, sizeof text, "%u MOVEMENT%s NEVER GET GREEN", left_out, left_out == 1 ? "" : "S");
            note(r, x, y, text, rgb(200, 120, 30));
        } else note(r, x, y, "AMBER MARK: A TURN HAS GREEN BUT GIVES WAY");
        ui_.end_panel();
    }

    void draw_panel(Renderer2D& r) {
        if (!app_) return;
        ui_.set_scale(ui_scale());
        ui_.begin(&r, app_->ui_input(), nullptr, skin());
        const Rect kPanel = panel();
        ui_.begin_panel(kPanel);
        const float left = kPanel.x + 10, top = kPanel.y + 10;

        // Tools, two by two.
        for (u32 i = 0; i < kToolCount; ++i) {
            const Rect box{left + static_cast<float>(i % 2) * 78, top + static_cast<float>(i / 2) * 28, 74, 24};
            if (ui_.button_at(box, kToolNames[i])) app_->push_command(kCmdTool, static_cast<i32>(i));
            outline_if(r, box, tool_ == i);
        }

        // How a road is drawn and what the clicks snap to.
        const float mid = left + 168;
        const char* shapes[2] = {"Line", "Curve"};
        for (u32 i = 0; i < 2; ++i) {
            const Rect box{mid + static_cast<float>(i) * 78, top, 74, 24};
            if (ui_.button_at(box, shapes[i])) app_->push_command(kCmdShape, static_cast<i32>(i));
            outline_if(r, box, shape_ == i);
        }
        const Rect grid_box{mid, top + 28, 74, 24}, angle_box{mid + 78, top + 28, 74, 24};
        if (ui_.button_at(grid_box, "Grid")) app_->push_command(kCmdGrid);
        outline_if(r, grid_box, snap_grid_);
        if (ui_.button_at(angle_box, "15 deg")) app_->push_command(kCmdAngle);
        outline_if(r, angle_box, snap_angle_);
        char text[48];
        std::snprintf(text, sizeof text, "Undo %zu", undo_.size());
        if (ui_.button_at({mid, top + 60, 74, 22}, text) && !undo_.empty()) app_->push_command(kCmdUndo);
        std::snprintf(text, sizeof text, "Redo %zu", redo_.size());
        if (ui_.button_at({mid + 78, top + 60, 74, 22}, text) && !redo_.empty()) app_->push_command(kCmdRedo);

        // The kinds of road, each drawn as a slab of itself.
        float x = mid + 168;
        for (u32 i = 0; i < road_types().size(); ++i) {
            const RoadType& type = road_types()[i];
            const Rect card{x, top, kCardWidth - 8, 86};
            if (ui_.button_at(card, "")) app_->push_command(kCmdType, static_cast<i32>(i));
            draw_type(r, Rect{card.x + 6, card.y + 6, card.w - 12, 46}, type);
            const float us = ui_scale();
            const int ts = static_cast<int>(us);
            r.draw_text((card.x + 7) * us, (card.y + 60) * us, type.name, rgb(46, 48, 54), ts);
            std::snprintf(text, sizeof text, "%.1f M", type.width());
            r.draw_text((card.x + 7) * us, (card.y + 72) * us, text, rgb(122, 124, 130), ts);
            outline_if(r, card, type_ == i);
            x += kCardWidth;
        }

        // Starting over.
        const float right = kPanel.x + kPanel.w - 86;
        if (ui_.button_at({right, top, 76, 24}, "Demo")) app_->push_command(kCmdDemo);
        if (ui_.button_at({right, top + 28, 76, 24}, "Clear")) app_->push_command(kCmdClear);
        const Rect cars_box{right, top + 60, 76, 22};
        if (ui_.button_at(cars_box, "Cars")) app_->push_command(kCmdCars);
        outline_if(r, cars_box, cars_on_);
        ui_.end_panel();

        // The clock: the time of day, and how fast it runs. Click to change the speed.
        const int minutes = static_cast<int>(clock_ / 60.0f);
        std::snprintf(text, sizeof text, "%02d:%02d %s", minutes / 60, minutes % 60, kClockNames[clock_rate_ % 3]);
        ui_.begin_panel(clock_box());
        if (ui_.button_at({clock_box().x + 3, clock_box().y + 3, clock_box().w - 6, clock_box().h - 6}, text)) app_->push_command(kCmdClockRate);
        ui_.end_panel();

        if (lights_open()) draw_lights_panel(r);
        if (lanes_open()) draw_lanes_panel(r);
        ui_.end();

        const float us = ui_scale();
        const int ts = static_cast<int>(us);
        r.draw_text(13 * us, 13 * us, hint(), rgba(0, 0, 0, 120), ts);
        r.draw_text(12 * us, 12 * us, hint(), rgba(250, 250, 244, 235), ts);
        usize roads_count = 0;
        float metres = 0;
        for (const NetEdge& e : net_.edges())
            if (e.alive) {
                ++roads_count;
                metres += e.path.length();
            }
        if (cars_on_) std::snprintf(text, sizeof text, "%zu ROADS   %.0f M   %zu CARS", roads_count, metres, cars_.count());
        else std::snprintf(text, sizeof text, "%zu ROADS   %.0f M", roads_count, metres);
        r.draw_text(12 * us, 28 * us, text, rgba(250, 250, 244, 170), ts);
    }

    /// A slab of a kind of road seen from above, to its own proportions.
    void draw_type(Renderer2D& r, const Rect& box, const RoadType& type) {
        const float scale = std::min(box.h / 22.0f, 2.4f); // pixels per metre: the widest road just fits
        const float mid = box.y + box.h * 0.5f;
        const float half = type.width() * 0.5f * scale;
        r.fill_rect(box.x, mid - half, box.w, half * 2, kAsphalt);
        for (usize i = 0; i < type.lanes.size(); ++i) {
            const Track& t = type.lanes[i];
            const float y = mid + (t.offset - t.width * 0.5f) * scale;
            if (i == 0) continue; // the kerb
            const bool centre = type.lanes[i - 1].forward != t.forward;
            if (centre) r.fill_rect(box.x, y - 1, box.w, 2, kPaint);
            else
                for (float dash = box.x + 3; dash < box.x + box.w - 6; dash += 14) r.fill_rect(dash, y - 0.5f, 7, 1, kPaint);
        }
    }

    // ---- state ----------------------------------------------------------------------------

    CurveNetwork net_;
    u32 tool_ = kToolBuild;
    u32 shape_ = static_cast<u32>(Shape::Straight);
    u32 type_ = 0;
    u32 demo_page_ = 0;
    bool snap_grid_ = false;
    bool snap_angle_ = false;
    bool show_lanes_ = false;

    bool drawing_ = false;
    Snap start_;        ///< where the road being drawn starts
    Snap target_;       ///< what the cursor is stuck to right now
    Vec2 shown_end_{};  ///< where the end of the road is drawn: pulled towards the target
    Vec2 cursor_{};
    float pulse_ = 1;   ///< seconds since the cursor caught something new
    u32 hover_node_ = kNone, hover_edge_ = kNone;
    u32 drag_node_ = kNone, drag_edge_ = kNone;
    std::string refusal_;
    std::vector<Snapshot> undo_, redo_;

    Signals signals_;
    u64 signals_version_ = 0;  ///< the network the lights were last checked against
    float clock_ = 7 * 3600.0f; ///< seconds since midnight
    u32 clock_rate_ = 0;
    u32 sel_node_ = kNone;     ///< the junction the lights tool is working on
    u32 sel_program_ = 0, sel_phase_ = 0;
    TrackRef sel_lane_{kNone, 0}, hover_lane_{kNone, 0}; ///< the lane the lanes tool is working on

    bool cars_on_ = false;
    NetworkAgents cars_;
    std::vector<Car> car_info_; ///< by agent index
    u64 cars_version_ = 0;      ///< the network the cars' routes were planned on
    float spawn_timer_ = 0;
    Rng rng_{20261120};

    Vec2 cam_center_{}, cam_target_{};
    float zoom_ = 4, zoom_target_ = 4;
    Vec2 zoom_anchor_{kWidth * 0.5f, kHeight * 0.5f};
    InputFrame prev_input_{};

    // Not state: how to draw it, and how the game was started.
    App* app_ = nullptr;
    float view_w_ = kWidth, view_h_ = kHeight;
    Ui ui_;
    RoadPaint paint_;
    u64 painted_ = 0;
    u64 painted_lights_ = 0;
    bool start_lights_ = false;
    std::vector<std::pair<Vec2, u32>> demo_lights_; ///< while an example map is being built
    Vec2 start_lights_at_{};
    bool start_demo_ = false;
    bool start_cars_ = false;
    bool start_gallery_ = false;
    float start_zoom_ = 4;
    Vec2 start_at_{};
};

REWIND_GAME(Roads)
