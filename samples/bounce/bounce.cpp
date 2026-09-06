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

struct Shape {
    float radius = 10;
    u8 color = 0;
    void reflect(Visitor& v) {
        v.field("radius", radius, {.min = 2, .max = 60});
        v.field("color", color, {.min = 0, .max = 5});
    }
};

class Bounce final : public Game {
public:
    Bounce() {
        // Pinned order: this is the layout of the saved state.
        world_.register_component<Transform>("Transform");
        world_.register_component<Velocity>("Velocity");
        world_.register_component<Shape>("Shape");
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

        if (pressed(in, prev_input_, Button::A)) spawn_ball();
        if (pressed(in, prev_input_, Button::B)) pop_ball();
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
                ctx.emit(kEvBounce);
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

        r.draw_text(12, kHeight - 60, "arrows / WASD: move   Z: launch a ball   X: pop one", rgb(160, 160, 180));
        r.draw_text(12, kHeight - 44, "hold F4: rewind   F2: pause   F3: step   F5: replay + verify", rgb(160, 160, 180));
        r.draw_text(12, kHeight - 28, "F1: toggle overlay   Esc: quit   wheel: zoom   right drag: pan", rgb(160, 160, 180));
    }

    World* world() override { return &world_; }

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
        world_.add(e, Velocity{rng_.range_f(-320.0f, 320.0f), rng_.range_f(-420.0f, -120.0f)});
        world_.add(e, Shape{rng_.range_f(8.0f, 26.0f), static_cast<u8>(rng_.below(kPaletteSize))});
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
    u32 bounces_ = 0;
    u64 tick_ = 0; // for the animated road markings only, not saved on purpose

    // Debug knob that is NOT part of the state, to demonstrate the desync detector.
    float gravity_ = 600.0f;
};

REWIND_GAME(Bounce)
