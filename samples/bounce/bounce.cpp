// Bounce: the smallest game that shows what Rewind is about.
//
// Move the square with the arrow keys or WASD. Press Z / J / Space to launch a
// ball from the square. Then hold F4 and watch everything run backwards, drag
// the timeline slider, or press F5 to replay the whole session from tick 0
// while the engine checks that every tick reproduces exactly.
//
// The balls are entities in a World with three components. The player is
// plain fields. Both styles are fine; both are reflected, so both rewind.
//
// Hot reload: run `rewind_run bounce_hot.dll`, change something below (a
// color, the gravity, the player speed), rebuild the bounce_hot target, and
// the running game picks up the new code without restarting.

#include <rewind/rewind.h>

#include <imgui.h>

#include <cmath>
#include <vector>

using namespace rw;

struct Transform {
    float x = 0, y = 0;
    void reflect(Visitor& v) {
        v.hint(Widget::Vec2); // two floats = a point: the inspector shows one row, the editor a move gizmo
        v.field("x", x);
        v.field("y", y);
    }
};

struct Velocity {
    float vx = 0, vy = 0;
    void reflect(Visitor& v) {
        v.field("vx", vx);
        v.field("vy", vy);
    }
};

/// A row of the ball_kinds table (samples/bounce/assets/ball_kinds.json):
/// what a launched ball can be. Edit the file while the game runs, or the
/// rows in the editor's Data panel, and the next launches use it.
struct BallKind {
    std::string id;
    float min_radius = 8;
    float max_radius = 26;
    float speed = 1;  ///< multiplies the launch velocity
    u8 color = 0;     ///< palette index
    AssetRef sprite;  ///< optional, a texture reference like any other row field
    void reflect(Visitor& v) {
        v.field("id", id);
        v.field("min_radius", min_radius, {.min = 2, .max = 60});
        v.field("max_radius", max_radius, {.min = 2, .max = 60});
        v.field("speed", speed, {.min = 0.1f, .max = 4, .speed = 0.05f});
        v.field("color", color, {.min = 0, .max = 5});
        v.field("sprite", sprite, {.asset_type = "texture"});
    }
};

struct Shape {
    float radius = 10;
    u8 color = 0;
    DefRef<BallKind> kind; ///< which row made this ball; the inspector offers the table's ids
    void reflect(Visitor& v) {
        v.field("radius", radius, {.min = 2, .max = 60});
        v.field("color", color, {.min = 0, .max = 5});
        v.field("kind", kind, {.table = "ball_kinds"});
    }
};

class Bounce final : public Game {
public:
    Bounce() {
        // Pinned order: this is the layout of the saved state.
        world_.register_component<Transform>("Transform");
        world_.register_component<Velocity>("Velocity");
        world_.register_component<Shape>("Shape");
        // Optional: an engine-provided parent link. The editor shows the
        // Entities panel as a tree and reparents by drag and drop.
        world_.register_component<Parent>("Parent");
    }

    void configure(AppConfig& cfg) override {
        cfg.title = "Rewind - bounce";
        cfg.width = static_cast<int>(kWidth);
        cfg.height = static_cast<int>(kHeight);
        cfg.timeline.tick_rate = 60;
        cfg.timeline.history_ticks = 60 * 120; // two minutes of rewind
    }

    void init() override {
        world_.clear();
        player_x_ = kWidth * 0.5f;
        player_y_ = kHeight * 0.5f;
        rng_ = Rng(1234);
        bounces_ = 0;
        for (int i = 0; i < 12; ++i) spawn_ball();
    }

    void tick(const TickContext& ctx) override {
        const float dt = ctx.dt;
        const InputFrame& in = ctx.input;
        tick_ = ctx.tick;

        // Player movement from the abstract axes, never from the keyboard.
        player_x_ += static_cast<float>(in.axis_x) / 32767.0f * kPlayerSpeed * dt;
        player_y_ += static_cast<float>(in.axis_y) / 32767.0f * kPlayerSpeed * dt;
        player_x_ = clampf(player_x_, 0.0f, kWidth - kPlayerSize);
        player_y_ = clampf(player_y_, 0.0f, kHeight - kPlayerSize);

        if (pressed(in, prev_input_, Button::A) || in.command == kCmdLaunch) spawn_ball();
        if (pressed(in, prev_input_, Button::B) || in.command == kCmdPop) pop_ball();
        // Camera intent is simulation state (it rewinds and replays); the
        // smooth motion towards it happens in render().
        if (in.wheel != 0) zoom_target_ = clampf(zoom_target_ * std::pow(1.25f, static_cast<float>(in.wheel)), 0.5f, 4.0f);
        if (in.down(Button::MouseRight) && prev_input_.down(Button::MouseRight)) {
            cam_target_.x -= static_cast<float>(in.mouse_x - prev_input_.mouse_x) / zoom_target_;
            cam_target_.y -= static_cast<float>(in.mouse_y - prev_input_.mouse_y) / zoom_target_;
        }
        prev_input_ = in;

        world_.each<Transform, Velocity, Shape>([&](Entity, Transform& t, Velocity& vel, Shape& s) {
            vel.vy += gravity_ * dt;
            t.x += vel.vx * dt;
            t.y += vel.vy * dt;

            if (t.x - s.radius < 0) { t.x = s.radius; vel.vx = -vel.vx; }
            if (t.x + s.radius > kWidth) { t.x = kWidth - s.radius; vel.vx = -vel.vx; }
            if (t.y - s.radius < 0) { t.y = s.radius; vel.vy = -vel.vy; }
            if (t.y + s.radius > kHeight) { t.y = kHeight - s.radius; vel.vy = -vel.vy * 0.95f; }

            // Bounce off the player: push out along the shortest axis.
            const float cx = clampf(t.x, player_x_, player_x_ + kPlayerSize);
            const float cy = clampf(t.y, player_y_, player_y_ + kPlayerSize);
            const float dx = t.x - cx;
            const float dy = t.y - cy;
            if (dx * dx + dy * dy < s.radius * s.radius) {
                if (absf(dx) > absf(dy)) {
                    vel.vx = dx >= 0 ? absf(vel.vx) + 40.0f : -absf(vel.vx) - 40.0f;
                    t.x = cx + (dx >= 0 ? s.radius : -s.radius);
                } else {
                    vel.vy = dy >= 0 ? absf(vel.vy) + 40.0f : -absf(vel.vy) - 40.0f;
                    t.y = cy + (dy >= 0 ? s.radius : -s.radius);
                }
                s.color = static_cast<u8>((s.color + 1) % kPaletteSize);
                ++bounces_;
                ctx.emit(kEvBounce, t.x, t.y);
            }
        });
    }

    // One description of the state. Saving, loading, the fingerprint and the
    // inspector in the overlay all come from this.
    void reflect(Visitor& v) override {
        v.field("player_x", player_x_, {.min = 0, .max = kWidth});
        v.field("player_y", player_y_, {.min = 0, .max = kHeight});
        v.field("world", world_);
        v.field("rng", rng_);
        v.field("prev_input", prev_input_);
        v.field("cam_target", cam_target_);
        v.field("network", net_);
        v.field("zoom_target", zoom_target_, {.min = 0.5f, .max = 4});
        v.field("bounces", bounces_);
        // Transient: shown in the inspector, but deliberately NOT part of the
        // saved state. See debug_ui().
        v.field("gravity", gravity_, {.min = 0, .max = 2000, .transient = true});
    }

    void render(Renderer2D& r, float /*alpha*/) override {
        r.clear(kBackground);

        // Smooth camera chasing the simulation's target; bounded to the arena.
        cam_.set_view_size(kWidth, kHeight);
        cam_.set_bounds({-kWidth * 0.5f, -kHeight * 0.5f, kWidth * 2, kHeight * 2});
        cam_.set_target(cam_target_);
        cam_.set_target_zoom(zoom_target_);
        cam_.update_realtime();
        r.set_camera(cam_.camera());

        // Spatial index demo: balls within reach of the player get a ring.
        // The grid is derived from the world each frame, never saved.
        SpatialGrid<Entity> nearby(64);
        world_.each<Transform, Shape>([&](Entity e, Transform& t, Shape& s) {
            nearby.insert(e, {t.x - s.radius, t.y - s.radius, s.radius * 2, s.radius * 2});
        });
        nearby.query_circle({player_x_ + kPlayerSize / 2, player_y_ + kPlayerSize / 2}, 140,
                            [&](u32, const Entity&, const Rect& b) {
                                r.draw_rect(b.x - 3, b.y - 3, b.w + 6, b.h + 6, rgba(255, 255, 255, 90));
                            });

        // Draw layers: the balls are recorded first but the track (layer -1)
        // still ends up underneath them.
        world_.each<Transform, Shape>([&](Entity, Transform& t, Shape& s) {
            r.set_sort_key(t.y + s.radius); // lower balls cover the ones behind them
            r.fill_circle(t.x, t.y, s.radius, kPalette[s.color % kPaletteSize]);
        });

        r.set_layer(-1);
        StrokeStyle road;
        road.width = 36;
        road.join = LineJoin::Round;
        road.cap = LineCap::Round;
        road.r = 0.16f, road.g = 0.16f, road.b = 0.22f;
        // A smooth closed road through a few points (core/curve.h), with
        // edge lines offset from it and animated dashes down the middle.
        const Path track = Path::from_catmull_rom(
            {{120, 140}, {420, 90}, {700, 260}, {1000, 120}, {1160, 400}, {900, 620}, {300, 560}}, true);
        r.draw_path(track, road);
        StrokeStyle edge;
        edge.width = 2;
        edge.closed = true;
        edge.r = 0.6f, edge.g = 0.6f, edge.b = 0.7f;
        r.draw_polyline(track.offset(16), edge);
        r.draw_polyline(track.offset(-16), edge);
        StrokeStyle marking;
        marking.width = 3;
        marking.dash = 24;
        marking.gap = 18;
        marking.dash_offset = -static_cast<float>(tick_ % 42); // scrolls the markings along the road
        marking.r = 0.9f, marking.g = 0.85f, marking.b = 0.5f;
        r.draw_path(track, marking);
        r.fill_polygon({{60, 660}, {200, 640}, {230, 700}, {150, 690}, {100, 710}}, rgba(75, 192, 192, 80));
        if (!net_.nodes().empty()) draw_network(r, net_);
        r.set_layer(0);

        r.fill_rect(player_x_, player_y_, kPlayerSize, kPlayerSize, rgb(240, 240, 240));
        r.draw_rect(player_x_, player_y_, kPlayerSize, kPlayerSize, rgb(120, 200, 255));
        StrokeStyle halo;
        halo.width = 2;
        halo.dash = 6;
        halo.gap = 6;
        halo.closed = true;
        halo.r = 0.47f, halo.g = 0.78f, halo.b = 1.0f;
        std::vector<Vec2> ring;
        for (int i = 0; i < 40; ++i) {
            const float ang = static_cast<float>(i) / 40 * 6.2831853f;
            ring.push_back({player_x_ + kPlayerSize / 2 + std::cos(ang) * 44, player_y_ + kPlayerSize / 2 + std::sin(ang) * 44});
        }
        r.draw_polyline(ring, halo);

        // Particles are presentation: fed by bounce events, stepped with the frame time.
        r.set_sort_key(0);
        particles_.update(app_ ? app_->stats().frame_ms / 1000.0f : 1.0f / 60);
        particles_.draw(r);

        draw_hud(r);
        r.draw_text(12, kHeight - 60, "arrows / WASD: move   Z: launch a ball   X: pop one", rgb(160, 160, 180));
        r.draw_text(12, kHeight - 44, "hold F4: rewind   F2: pause   F3: step   F5: replay + verify", rgb(160, 160, 180));
        r.draw_text(12, kHeight - 28, "F1: toggle overlay   Esc: quit   wheel: zoom   right drag: pan", rgb(160, 160, 180));
    }

    World* world() override { return &world_; }

    /// A hot reload that kept the state builds a new game object that never
    /// saw start(), so everything start() attaches has to be attached again.
    /// Everything it does is idempotent, so running it again is the whole job.
    void reloaded(App& app) override { start(app); }

    void start(App& app) override {
        app_ = &app;
        std::string error;
        if (!kinds_.load(&error)) RW_LOG_WARN("bounce", "%s (launching plain balls)", error.c_str());
    }

    std::vector<DataTableBase*> data_tables() override { return {&kinds_}; }

    /// Sparks where a ball hit the player. Events re-fire on replay, so the
    /// sparks come back with the bounce.
    void on_event(const SimEvent& e, Audio&) override {
        if (e.id == kEvBounce) particles_.emit(spark_, {e.a, e.b});
    }

    /// HUD with the game UI: presentation, so it runs after the world is
    /// drawn; clicks that change the simulation go in as commands.
    void draw_hud(Renderer2D& r) {
        if (!app_) return;
        Ui& ui = ui_;
        ui.begin(&r, app_->ui_input());
        ui.begin_panel({kWidth - 262, 12, 250, 150}, "Bounce");
        char line[64];
        std::snprintf(line, sizeof line, "%zu balls  %u hits", world_.entity_count(), bounces_);
        ui.label(line, true);
        if (ui.button("Launch")) app_->push_command(kCmdLaunch);
        ui.tooltip("same as Z: goes through the input record");
        ui.same_line();
        if (ui.button("Pop")) app_->push_command(kCmdPop);
        TimeControl& t = app_->time_control();
        const float scales[3] = {1, 2, 4};
        const char* names[3] = {"1x", "2x", "4x"};
        for (int i = 0; i < 3; ++i) {
            if (i > 0) ui.same_line();
            if (ui.button(names[i], 56)) t.time_scale = scales[i];
        }
        ui.progress(static_cast<float>(world_.entity_count()) / 40.0f, 234, {0, 0, 0, 0}, "capacity");
        ui.end_panel();
        ui.end();
        hud_wants_mouse_ = ui.wants_mouse();
    }

    // The editor's Scene panel offers this next to Select: draw a curve
    // network with the mouse. It is reflected state like everything else.
    std::vector<EditorTool*> editor_tools() override { return {&net_tool_}; }

    void debug_ui() override {
        ImGui::Text("entities %zu   bounces %u", world_.entity_count(), bounces_);
        ImGui::SliderFloat("gravity", &gravity_, 0.0f, 2000.0f, "%.0f");
        ImGui::TextWrapped("gravity is intentionally transient. Change it, then press F5: the replay "
                           "diverges and the desync detector above catches the exact tick.");
        if (ImGui::Button("save world.json")) {
            std::string error;
            if (!save_json_file("bounce_world.json", *this, &error)) ImGui::OpenPopup("save failed");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(whole state as text, next to the executable)");
    }

private:
    enum Event : u32 { kEvBounce = 1 };
    enum Command : u32 { kCmdLaunch = 1, kCmdPop = 2 };

    static constexpr float kWidth = 1280;
    static constexpr float kHeight = 720;
    static constexpr float kPlayerSize = 48;
    static constexpr float kPlayerSpeed = 420;
    static constexpr Color kBackground = rgb(18, 18, 26);
    static constexpr int kPaletteSize = 6;
    static constexpr Color kPalette[kPaletteSize] = {
        rgb(255, 99, 132), rgb(255, 205, 86), rgb(75, 192, 192),
        rgb(54, 162, 235), rgb(153, 102, 255), rgb(255, 159, 64),
    };

    void spawn_ball() {
        const Entity e = world_.create();
        world_.add(e, Transform{player_x_ + kPlayerSize * 0.5f, player_y_ + kPlayerSize * 0.5f});
        // Definitions are data: the table decides what a ball can be, the
        // deterministic rng picks which. A missing table means plain balls.
        if (kinds_.size() > 0) {
            const BallKind& kind = kinds_.at(rng_.below(static_cast<u32>(kinds_.size())));
            world_.add(e, Velocity{rng_.range_f(-320.0f, 320.0f) * kind.speed, rng_.range_f(-420.0f, -120.0f) * kind.speed});
            world_.add(e, Shape{rng_.range_f(kind.min_radius, kind.max_radius), kind.color, DefRef<BallKind>{kind.id}});
        } else {
            world_.add(e, Velocity{rng_.range_f(-320.0f, 320.0f), rng_.range_f(-420.0f, -120.0f)});
            world_.add(e, Shape{rng_.range_f(8.0f, 26.0f), static_cast<u8>(rng_.below(kPaletteSize))});
        }
    }

    void pop_ball() {
        // Remove the ball closest to the player, if any.
        Entity best = Entity::null();
        float best_d = 1e30f;
        const float px = player_x_ + kPlayerSize * 0.5f, py = player_y_ + kPlayerSize * 0.5f;
        world_.each<Transform>([&](Entity e, Transform& t) {
            const float d = (t.x - px) * (t.x - px) + (t.y - py) * (t.y - py);
            if (d < best_d) {
                best_d = d;
                best = e;
            }
        });
        world_.destroy(best);
    }

    // Simulation state: everything here is listed in reflect().
    float player_x_ = 0;
    float player_y_ = 0;
    World world_;
    Rng rng_;
    InputFrame prev_input_;
    Vec2 cam_target_{kWidth * 0.5f, kHeight * 0.5f};
    float zoom_target_ = 1;
    CameraController cam_; // presentation only
    CurveNetwork net_;
    NetworkTool net_tool_{&net_, "Network"};
    App* app_ = nullptr;
    DataTable<BallKind> kinds_{"ball_kinds", "ball_kinds.json"};
    ParticleSystem particles_{42};
    ParticleDef spark_ = [] {
        ParticleDef d;
        d.id = "spark";
        d.lifetime_min = 0.25f;
        d.lifetime_max = 0.6f;
        d.speed_min = 60;
        d.speed_max = 220;
        d.gravity = {0, 400};
        d.drag = 1.5f;
        d.size_start = 5;
        d.size_end = 0;
        d.color_start = rgb(255, 235, 160);
        d.color_end = rgba(255, 120, 60, 0);
        d.burst = 14;
        return d;
    }();
    Ui ui_;                  // presentation only
    bool hud_wants_mouse_ = false;
    u32 bounces_ = 0;
    u64 tick_ = 0; // for the animated road markings only, not saved on purpose

    // Debug knob that is NOT part of the state, to demonstrate the desync detector.
    float gravity_ = 600.0f;
};

REWIND_GAME(Bounce)
