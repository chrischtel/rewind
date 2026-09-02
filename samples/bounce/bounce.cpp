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

#include <vector>

using namespace rw;

struct Transform {
    float x = 0, y = 0;
    void reflect(Visitor& v) {
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

        // Player movement from the abstract axes, never from the keyboard.
        player_x_ += static_cast<float>(in.axis_x) / 32767.0f * kPlayerSpeed * dt;
        player_y_ += static_cast<float>(in.axis_y) / 32767.0f * kPlayerSpeed * dt;
        player_x_ = clampf(player_x_, 0.0f, kWidth - kPlayerSize);
        player_y_ = clampf(player_y_, 0.0f, kHeight - kPlayerSize);

        if (pressed(in, prev_input_, Button::A)) spawn_ball();
        if (pressed(in, prev_input_, Button::B)) pop_ball();
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
        v.field("bounces", bounces_);
        // Transient: shown in the inspector, but deliberately NOT part of the
        // saved state. See debug_ui().
        v.field("gravity", gravity_, {.min = 0, .max = 2000, .transient = true});
    }

    void render(Renderer2D& r, float /*alpha*/) override {
        r.clear(kBackground);

        world_.each<Transform, Shape>([&](Entity, Transform& t, Shape& s) {
            r.fill_circle(t.x, t.y, s.radius, kPalette[s.color % kPaletteSize]);
        });

        r.fill_rect(player_x_, player_y_, kPlayerSize, kPlayerSize, rgb(240, 240, 240));
        r.draw_rect(player_x_, player_y_, kPlayerSize, kPlayerSize, rgb(120, 200, 255));

        r.draw_text(12, kHeight - 60, "arrows / WASD: move   Z: launch a ball   X: pop one", rgb(160, 160, 180));
        r.draw_text(12, kHeight - 44, "hold F4: rewind   F2: pause   F3: step   F5: replay + verify", rgb(160, 160, 180));
        r.draw_text(12, kHeight - 28, "F1: toggle overlay   Esc: quit", rgb(160, 160, 180));
    }

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
    u32 bounces_ = 0;

    // Debug knob that is NOT part of the state, to demonstrate the desync detector.
    float gravity_ = 600.0f;
};

REWIND_GAME(Bounce)
