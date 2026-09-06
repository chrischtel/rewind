// Rewind Defense: a tower defense built on the engine's entity world.
//
// Enemies walk the path from the red gate to the blue gate. Build towers on
// the grass with the mouse, upgrade them by clicking again, sell them with
// the right button. Waves escalate; a boss arrives every tenth wave. Lose all
// lives and the run is over... unless you hold F4 and take the last wave back.
//
//   mouse          aim, build / upgrade (left), sell (right)
//   X / B button   cycle the tower to build
//   Enter / Start  call the next wave early for bonus gold
//
// Everything below runs through the deterministic tick: the mouse is part of
// the input frame, so a replay or a rewind reproduces every click.

#include <rewind/rewind.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace rw;

namespace {

constexpr int kTile = 32;
constexpr int kCols = 20;
constexpr int kRows = 11;
constexpr int kViewW = kCols * kTile;              // 640
constexpr int kMapTop = 12;                        // HUD strip above the map
constexpr int kMapBottom = kMapTop + kRows * kTile; // 362
constexpr int kViewH = kMapBottom + 20;            // tower bar below the map: 384, scales 2x into 1280x768

// '#' path, '.' buildable, 'S' spawn, 'E' exit, '~' water (not buildable).
const std::vector<std::string> kMap = {
    "S###.....~~.........",
    "...#.....~~..######.",
    "...#..####...#....#.",
    "...#..#..#...#....#.",
    "...####..#####....#.",
    ".........~~.......#.",
    ".######..~~..######.",
    ".#....#......#......",
    ".#....########......",
    ".#..................",
    ".###################E",
};

enum class TowerKind : u8 { Arrow = 0, Cannon = 1, Frost = 2 };
constexpr int kTowerKinds = 3;

struct TowerSpec {
    const char* name;
    i32 cost;
    float range;
    float damage;
    float cooldown; ///< seconds between shots
    float bullet_speed;
    float splash;   ///< radius, 0 = single target
    float slow;     ///< seconds of slow applied, 0 = none
    Color color;
};

constexpr TowerSpec kTowers[kTowerKinds] = {
    {"Arrow", 60, 96, 11, 0.32f, 460, 0, 0, rgb(90, 170, 255)},
    {"Cannon", 120, 88, 38, 1.35f, 260, 36, 0, rgb(255, 160, 70)},
    {"Frost", 90, 80, 5, 0.55f, 380, 0, 1.8f, rgb(130, 230, 255)},
};

constexpr const char* kTowerNames[kTowerKinds] = {"Arrow", "Cannon", "Frost"};

enum class EnemyKind : u8 { Grunt = 0, Runner = 1, Tank = 2, Boss = 3 };
constexpr const char* kEnemyNames[4] = {"Grunt", "Runner", "Tank", "Boss"};

struct EnemySpec {
    float hp;
    float speed;
    i32 bounty;
    float radius;
    Color color;
    i32 damage; ///< lives lost when it reaches the exit
};

constexpr EnemySpec kEnemies[4] = {
    {34, 58, 6, 7, rgb(220, 90, 90), 1},
    {18, 110, 5, 5, rgb(250, 220, 90), 1},
    {140, 36, 18, 10, rgb(150, 100, 200), 2},
    {900, 30, 120, 14, rgb(255, 60, 160), 5},
};

enum Event : u32 { kEvShoot = 1, kEvHit, kEvKill, kEvLeak, kEvBuild, kEvSell, kEvWave, kEvGameOver, kEvDeny };

// ---- components ------------------------------------------------------------------

struct Transform {
    float x = 0, y = 0;
    void reflect(Visitor& v) {
        v.field("x", x);
        v.field("y", y);
    }
};

struct Enemy {
    u8 kind = 0;
    float hp = 1;
    float max_hp = 1;
    u16 waypoint = 1; ///< index of the waypoint being walked towards
    float slow = 0;   ///< seconds of slow remaining
    float travelled = 0; ///< distance along the path, for "first" targeting
    void reflect(Visitor& v) {
        v.field("kind", kind, {.names = kEnemyNames, .name_count = 4});
        v.field("hp", hp);
        v.field("max_hp", max_hp);
        v.field("waypoint", waypoint);
        v.field("slow", slow);
        v.field("travelled", travelled);
    }
};

struct Tower {
    u8 kind = 0;
    u8 level = 1;
    float cooldown = 0;
    i32 invested = 0; ///< gold spent, for selling
    void reflect(Visitor& v) {
        v.field("kind", kind, {.names = kTowerNames, .name_count = kTowerKinds});
        v.field("level", level, {.min = 1, .max = 3});
        v.field("cooldown", cooldown);
        v.field("invested", invested);
    }
};

struct Projectile {
    u8 kind = 0;
    Entity target;
    float damage = 0;
    float speed = 0;
    float aim_x = 0, aim_y = 0; ///< last known target position
    void reflect(Visitor& v) {
        v.field("kind", kind);
        v.field("target", target);
        v.field("damage", damage);
        v.field("speed", speed);
        v.field("aim_x", aim_x);
        v.field("aim_y", aim_y);
    }
};

struct Particle {
    float vx = 0, vy = 0;
    float life = 0;
    Color color;
    void reflect(Visitor& v) {
        v.field("vx", vx);
        v.field("vy", vy);
        v.field("life", life);
        v.field("color", color);
    }
};

/// A short synthesized sound: square wave sweep with decay and optional noise.
std::vector<float> synth(float seconds, float f0, float f1, float noise, u32 seed) {
    const int rate = 48000;
    const int n = static_cast<int>(seconds * rate);
    std::vector<float> out(static_cast<usize>(n));
    Rng rng(seed);
    float phase = 0;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        const float f = f0 + (f1 - f0) * t;
        phase += f / rate;
        const float tone = phase - std::floor(phase) < 0.5f ? 0.6f : -0.6f;
        const float hiss = (rng.unit() * 2.0f - 1.0f) * noise;
        out[static_cast<usize>(i)] = (tone * (1.0f - noise) + hiss) * (1.0f - t) * (1.0f - t) * 0.5f;
    }
    return out;
}

float dist(float ax, float ay, float bx, float by) {
    const float dx = ax - bx, dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

class TowerDefense final : public Game {
public:
    TowerDefense() {
        world_.register_component<Transform>("Transform");
        world_.register_component<Enemy>("Enemy");
        world_.register_component<Tower>("Tower");
        world_.register_component<Projectile>("Projectile");
        world_.register_component<Particle>("Particle");
        build_waypoints();
    }

    void configure(AppConfig& cfg) override {
        cfg.title = "Rewind Defense";
        cfg.width = 1280;
        cfg.height = 768;
        cfg.render_width = kViewW;
        cfg.render_height = kViewH;
        cfg.integer_scale = true;
        cfg.pixel_art = true;
        cfg.timeline.history_ticks = 60 * 180; // three minutes: enough to take back a whole wave
    }

    void load_assets(Renderer2D& r, Audio& audio) override {
        // A real font for the HUD when the system has one; the pixel font otherwise.
        const char* fonts[] = {"C:/Windows/Fonts/consolab.ttf", "C:/Windows/Fonts/consola.ttf",
                               "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"};
        font_ = nullptr;
        for (const char* f : fonts) {
            if (!std::filesystem::exists(f)) continue;
            font_ = r.load_font(f, 12.0f);
            title_font_ = r.load_font(f, 28.0f);
            if (font_ && font_->ok) break;
        }

        auto make = [&](const char* name, float sec, float f0, float f1, float noise, u32 seed) {
            const std::vector<float> s = synth(sec, f0, f1, noise, seed);
            return audio.create_sound(name, 48000, 1, s.data(), s.size());
        };
        snd_shoot_ = make("shoot", 0.06f, 900, 500, 0.1f, 1);
        snd_hit_ = make("hit", 0.05f, 300, 120, 0.7f, 2);
        snd_kill_ = make("kill", 0.14f, 700, 200, 0.2f, 3);
        snd_leak_ = make("leak", 0.35f, 160, 60, 0.3f, 4);
        snd_build_ = make("build", 0.12f, 300, 800, 0.0f, 5);
        snd_sell_ = make("sell", 0.12f, 800, 300, 0.0f, 6);
        snd_wave_ = make("wave", 0.4f, 400, 900, 0.0f, 7);
        snd_over_ = make("over", 0.9f, 300, 40, 0.4f, 8);
        snd_deny_ = make("deny", 0.08f, 200, 150, 0.2f, 9);
    }

    void init() override {
        world_.clear();
        gold_ = 150;
        lives_ = 20;
        wave_ = 0;
        score_ = 0;
        wave_active_ = false;
        countdown_ = 8.0f;
        to_spawn_ = 0;
        spawn_timer_ = 0;
        game_over_ = false;
        selected_ = 0;
        elapsed_ = 0;
        rng_ = Rng(2024);
        prev_ = InputFrame{};
    }

    // ---- simulation ----------------------------------------------------------------

    void tick(const TickContext& c) override {
        RW_PROFILE_SCOPE("td.tick");
        const float dt = c.dt;
        const InputFrame& in = c.input;
        elapsed_ += dt;

        if (!game_over_) {
            handle_build_input(in, c);
            update_waves(dt, c);
            update_towers(dt, c);
            update_projectiles(dt, c);
            update_enemies(dt, c);
        }
        update_particles(dt);
        prev_ = in;
    }

    void handle_build_input(const InputFrame& in, const TickContext& c) {
        if (pressed(in, prev_, Button::B)) selected_ = static_cast<u8>((selected_ + 1) % kTowerKinds);
        if (pressed(in, prev_, Button::Start) && !wave_active_) {
            gold_ += static_cast<i32>(countdown_) * 3; // early-call bonus
            countdown_ = 0;
        }

        const int tx = in.mouse_x / kTile;
        const int ty = (in.mouse_y - kMapTop) / kTile;
        const bool on_map = in.mouse_x >= 0 && in.mouse_y >= kMapTop && tx < kCols && ty < kRows;
        if (!on_map) return;
        const Entity here = tower_at(tx, ty);

        if (pressed(in, prev_, Button::MouseLeft)) {
            if (world_.alive(here)) {
                Tower& t = *world_.get<Tower>(here);
                const i32 cost = upgrade_cost(t);
                if (t.level < 3 && gold_ >= cost) {
                    gold_ -= cost;
                    t.invested += cost;
                    ++t.level;
                    c.emit(kEvBuild);
                } else {
                    c.emit(kEvDeny);
                }
            } else if (buildable(tx, ty)) {
                const TowerSpec& spec = kTowers[selected_];
                if (gold_ >= spec.cost) {
                    gold_ -= spec.cost;
                    const Entity e = world_.create();
                    world_.add(e, Transform{tile_center_x(tx), tile_center_y(ty)});
                    world_.add(e, Tower{selected_, 1, 0, spec.cost});
                    c.emit(kEvBuild);
                } else {
                    c.emit(kEvDeny);
                }
            }
        }
        if (pressed(in, prev_, Button::MouseRight) && world_.alive(here)) {
            gold_ += world_.get<Tower>(here)->invested * 6 / 10;
            world_.destroy(here);
            c.emit(kEvSell);
        }
    }

    void update_waves(float dt, const TickContext& c) {
        if (!wave_active_) {
            countdown_ -= dt;
            if (countdown_ <= 0) start_wave(c);
            return;
        }
        if (to_spawn_ > 0) {
            spawn_timer_ -= dt;
            if (spawn_timer_ <= 0) {
                spawn_enemy();
                --to_spawn_;
                spawn_timer_ = spawn_interval();
            }
        } else if (world_.count<Enemy>() == 0) {
            // Wave cleared.
            wave_active_ = false;
            countdown_ = 12.0f;
            gold_ += 20 + wave_ * 4;
            score_ += wave_ * 100;
        }
    }

    void start_wave(const TickContext& c) {
        ++wave_;
        wave_active_ = true;
        to_spawn_ = 6 + wave_ * 2 + (wave_ % 10 == 0 ? 1 : 0);
        spawn_timer_ = 0.2f;
        spawned_ = 0;
        c.emit(kEvWave);
    }

    float spawn_interval() const { return std::max(0.35f, 0.9f - wave_ * 0.03f); }

    void spawn_enemy() {
        EnemyKind kind = EnemyKind::Grunt;
        const u32 n = spawned_++;
        if (wave_ % 10 == 0 && n == 0) kind = EnemyKind::Boss;
        else if (wave_ >= 5 && n % 5 == 4) kind = EnemyKind::Tank;
        else if (wave_ >= 3 && n % 3 == 2) kind = EnemyKind::Runner;

        const EnemySpec& spec = kEnemies[static_cast<usize>(kind)];
        const float scale = 1.0f + 0.16f * static_cast<float>(wave_ - 1);
        const Entity e = world_.create();
        world_.add(e, Transform{waypoints_[0].x, waypoints_[0].y});
        Enemy en;
        en.kind = static_cast<u8>(kind);
        en.max_hp = spec.hp * scale;
        en.hp = en.max_hp;
        en.waypoint = 1;
        world_.add(e, en);
    }

    void update_enemies(float dt, const TickContext& c) {
        std::vector<Entity> leaked;
        world_.each<Transform, Enemy>([&](Entity e, Transform& t, Enemy& en) {
            const EnemySpec& spec = kEnemies[en.kind];
            float speed = spec.speed;
            if (en.slow > 0) {
                en.slow -= dt;
                speed *= 0.45f;
            }
            float remaining = speed * dt;
            while (remaining > 0 && en.waypoint < waypoints_.size()) {
                const Vec2 target = waypoints_[en.waypoint];
                const float d = dist(t.x, t.y, target.x, target.y);
                if (d <= remaining) {
                    t.x = target.x;
                    t.y = target.y;
                    remaining -= d;
                    en.travelled += d;
                    ++en.waypoint;
                } else {
                    t.x += (target.x - t.x) / d * remaining;
                    t.y += (target.y - t.y) / d * remaining;
                    en.travelled += remaining;
                    remaining = 0;
                }
            }
            if (en.waypoint >= waypoints_.size()) leaked.push_back(e);
        });
        for (Entity e : leaked) {
            const Enemy& en = *world_.get<Enemy>(e);
            lives_ -= kEnemies[en.kind].damage;
            world_.destroy(e);
            c.emit(kEvLeak);
            if (lives_ <= 0 && !game_over_) {
                lives_ = 0;
                game_over_ = true;
                c.emit(kEvGameOver);
            }
        }
    }

    void update_towers(float dt, const TickContext& c) {
        world_.each<Transform, Tower>([&](Entity, Transform& t, Tower& tw) {
            tw.cooldown -= dt;
            if (tw.cooldown > 0) return;
            const TowerSpec& spec = kTowers[tw.kind];
            const float range = tower_range(tw);

            // "First" targeting: the enemy furthest along the path within range.
            Entity best = Entity::null();
            float best_travel = -1;
            float bx = 0, by = 0;
            world_.each<Transform, Enemy>([&](Entity e, Transform& et, Enemy& en) {
                if (dist(t.x, t.y, et.x, et.y) > range) return;
                if (en.travelled > best_travel) {
                    best_travel = en.travelled;
                    best = e;
                    bx = et.x;
                    by = et.y;
                }
            });
            if (best.is_null()) return;

            const Entity p = world_.create();
            world_.add(p, Transform{t.x, t.y});
            Projectile pr;
            pr.kind = tw.kind;
            pr.target = best;
            pr.damage = tower_damage(tw);
            pr.speed = spec.bullet_speed;
            pr.aim_x = bx;
            pr.aim_y = by;
            world_.add(p, pr);
            tw.cooldown = spec.cooldown;
            c.emit(kEvShoot, static_cast<float>(tw.kind));
        });
    }

    void update_projectiles(float dt, const TickContext& c) {
        std::vector<Entity> done;
        world_.each<Transform, Projectile>([&](Entity e, Transform& t, Projectile& p) {
            if (const Transform* tt = world_.get<Transform>(p.target)) {
                p.aim_x = tt->x;
                p.aim_y = tt->y;
            }
            const float d = dist(t.x, t.y, p.aim_x, p.aim_y);
            const float step = p.speed * dt;
            if (d <= step + 2.0f) {
                t.x = p.aim_x;
                t.y = p.aim_y;
                done.push_back(e);
                return;
            }
            t.x += (p.aim_x - t.x) / d * step;
            t.y += (p.aim_y - t.y) / d * step;
        });

        for (Entity e : done) {
            const Projectile p = *world_.get<Projectile>(e);
            const Transform at = *world_.get<Transform>(e);
            world_.destroy(e);
            const TowerSpec& spec = kTowers[p.kind];

            std::vector<Entity> hit;
            if (spec.splash > 0) {
                world_.each<Transform, Enemy>([&](Entity en, Transform& et, Enemy&) {
                    if (dist(at.x, at.y, et.x, et.y) <= spec.splash) hit.push_back(en);
                });
                burst(at.x, at.y, spec.color, 10);
            } else if (world_.alive(p.target)) {
                hit.push_back(p.target);
            }
            if (!hit.empty()) c.emit(kEvHit);
            for (Entity en : hit) damage_enemy(en, p.damage, spec.slow, c);
        }
    }

    void damage_enemy(Entity e, float damage, float slow, const TickContext& c) {
        Enemy* en = world_.get<Enemy>(e);
        if (!en) return;
        en->hp -= damage;
        if (slow > 0) en->slow = std::max(en->slow, slow);
        if (en->hp > 0) return;
        const Transform t = *world_.get<Transform>(e);
        const EnemySpec& spec = kEnemies[en->kind];
        gold_ += spec.bounty;
        score_ += spec.bounty * 10;
        burst(t.x, t.y, spec.color, en->kind == 3 ? 40 : 8);
        world_.destroy(e);
        c.emit(kEvKill, static_cast<float>(spec.bounty));
    }

    void burst(float x, float y, Color color, int count) {
        for (int i = 0; i < count; ++i) {
            const Entity e = world_.create();
            world_.add(e, Transform{x, y});
            world_.add(e, Particle{rng_.range_f(-90, 90), rng_.range_f(-110, 40), rng_.range_f(0.25f, 0.6f), color});
        }
    }

    void update_particles(float dt) {
        std::vector<Entity> dead;
        world_.each<Transform, Particle>([&](Entity e, Transform& t, Particle& p) {
            p.vy += 260.0f * dt;
            t.x += p.vx * dt;
            t.y += p.vy * dt;
            p.life -= dt;
            if (p.life <= 0) dead.push_back(e);
        });
        for (Entity e : dead) world_.destroy(e);
    }

    // ---- events --------------------------------------------------------------------

    void on_event(const SimEvent& e, Audio& audio) override {
        switch (e.id) {
        case kEvShoot: audio.play(snd_shoot_, e.a == 1 ? 0.35f : 0.18f); break;
        case kEvHit: audio.play(snd_hit_, 0.25f); break;
        case kEvKill: audio.play(snd_kill_, 0.35f); break;
        case kEvLeak: audio.play(snd_leak_, 0.6f); break;
        case kEvBuild: audio.play(snd_build_, 0.5f); break;
        case kEvSell: audio.play(snd_sell_, 0.5f); break;
        case kEvWave: audio.play(snd_wave_, 0.5f); break;
        case kEvGameOver: audio.play(snd_over_, 0.8f); break;
        case kEvDeny: audio.play(snd_deny_, 0.4f); break;
        default: break;
        }
    }

    // ---- state ---------------------------------------------------------------------

    void reflect(Visitor& v) override {
        v.field("gold", gold_);
        v.field("lives", lives_, {.min = 0, .max = 20});
        v.field("wave", wave_);
        v.field("score", score_);
        v.field("wave_active", wave_active_);
        v.field("countdown", countdown_);
        v.field("to_spawn", to_spawn_);
        v.field("spawned", spawned_);
        v.field("spawn_timer", spawn_timer_);
        v.field("game_over", game_over_);
        v.field("selected", selected_, {.min = 0, .max = kTowerKinds - 1});
        v.field("elapsed", elapsed_);
        v.field("rng", rng_);
        v.field("prev_input", prev_);
        v.field("world", world_);
    }

    // ---- rendering -----------------------------------------------------------------

    void render(Renderer2D& r, float) override {
        RW_PROFILE_SCOPE("td.render");
        r.clear(rgb(24, 30, 26));
        draw_map(r);
        draw_entities(r);
        draw_cursor(r);
        draw_hud(r);
    }

    void draw_map(Renderer2D& r) {
        for (int y = 0; y < kRows; ++y) {
            for (int x = 0; x < kCols; ++x) {
                const char c = kMap[static_cast<usize>(y)][static_cast<usize>(x)];
                Color color = ((x + y) % 2 == 0) ? rgb(44, 82, 52) : rgb(40, 76, 48);
                if (c == '#') color = rgb(150, 124, 84);
                if (c == '~') color = ((x + y) % 2 == 0) ? rgb(40, 70, 120) : rgb(36, 64, 112);
                if (c == 'S') color = rgb(200, 70, 70);
                if (c == 'E') color = rgb(70, 110, 220);
                r.fill_rect(static_cast<float>(x * kTile), static_cast<float>(kMapTop + y * kTile), kTile, kTile, color);
            }
        }
        // Path edges, for a little depth.
        for (usize i = 1; i < waypoints_.size(); ++i) {
            r.draw_line(waypoints_[i - 1].x, waypoints_[i - 1].y, waypoints_[i].x, waypoints_[i].y, rgba(255, 240, 200, 40));
        }
    }

    void draw_entities(Renderer2D& r) {
        const float now = elapsed_;
        world_.each<Transform, Tower>([&](Entity, Transform& t, Tower& tw) {
            const TowerSpec& spec = kTowers[tw.kind];
            const float half = kTile * 0.5f - 4;
            r.fill_rect(t.x - half, t.y - half, half * 2, half * 2, rgb(30, 30, 40));
            r.fill_rect(t.x - half + 3, t.y - half + 3, half * 2 - 6, half * 2 - 6, spec.color);
            for (int i = 0; i < tw.level; ++i)
                r.fill_rect(t.x - half + 3 + static_cast<float>(i) * 5, t.y + half - 6, 3, 3, rgb(255, 255, 255));
            if (tw.cooldown > kTowers[tw.kind].cooldown - 0.06f)
                r.fill_circle(t.x, t.y, 5, rgba(255, 255, 255, 180), 8); // muzzle flash
        });

        world_.each<Transform, Projectile>([&](Entity, Transform& t, Projectile& p) {
            const TowerSpec& spec = kTowers[p.kind];
            r.fill_circle(t.x, t.y, spec.splash > 0 ? 4.0f : 2.5f, spec.color, 8);
        });

        world_.each<Transform, Enemy>([&](Entity, Transform& t, Enemy& en) {
            const EnemySpec& spec = kEnemies[en.kind];
            const float bob = std::sin(now * 9.0f + t.x * 0.1f) * 1.5f;
            Color body = spec.color;
            if (en.slow > 0) body = rgb(160, 220, 255);
            r.fill_circle(t.x, t.y + bob, spec.radius + 1, rgb(20, 20, 30), 12);
            r.fill_circle(t.x, t.y + bob, spec.radius, body, 12);
            const float w = spec.radius * 2.4f;
            const float frac = std::max(0.0f, en.hp / en.max_hp);
            r.fill_rect(t.x - w * 0.5f, t.y - spec.radius - 6 + bob, w, 3, rgb(40, 10, 10));
            r.fill_rect(t.x - w * 0.5f, t.y - spec.radius - 6 + bob, w * frac, 3, rgb(120, 255, 120));
        });

        world_.each<Transform, Particle>([&](Entity, Transform& t, Particle& p) {
            Color c = p.color;
            c.a = static_cast<u8>(std::min(255.0f, p.life * 500.0f));
            r.fill_rect(t.x - 1, t.y - 1, 2, 2, c);
        });
    }

    void draw_cursor(Renderer2D& r) {
        const int mx = prev_.mouse_x, my = prev_.mouse_y;
        const int tx = mx / kTile, ty = (my - kMapTop) / kTile;
        if (mx < 0 || my < kMapTop || tx >= kCols || ty >= kRows) return;
        const Entity here = tower_at(tx, ty);
        const float x = static_cast<float>(tx * kTile), y = static_cast<float>(kMapTop + ty * kTile);
        if (world_.alive(here)) {
            const Tower& tw = *world_.get<Tower>(here);
            const Transform& t = *world_.get<Transform>(here);
            draw_ring(r, t.x, t.y, tower_range(tw), rgba(255, 255, 255, 90));
            r.draw_rect(x, y, kTile, kTile, rgb(255, 255, 255));
        } else {
            const bool ok = buildable(tx, ty) && gold_ >= kTowers[selected_].cost;
            draw_ring(r, tile_center_x(tx), tile_center_y(ty), kTowers[selected_].range, rgba(255, 255, 255, 50));
            r.draw_rect(x, y, kTile, kTile, ok ? rgb(120, 255, 120) : rgb(255, 90, 90));
        }
    }

    static void draw_ring(Renderer2D& r, float cx, float cy, float radius, Color c) {
        constexpr int kSegments = 40;
        for (int i = 0; i < kSegments; ++i) {
            const float a0 = static_cast<float>(i) / kSegments * 6.2831853f;
            const float a1 = static_cast<float>(i + 1) / kSegments * 6.2831853f;
            r.draw_line(cx + std::cos(a0) * radius, cy + std::sin(a0) * radius, cx + std::cos(a1) * radius,
                        cy + std::sin(a1) * radius, c);
        }
    }

    void draw_hud(Renderer2D& r) {
        char buf[160];
        const bool ttf = font_ && font_->ok;
        auto text = [&](float x, float y, const char* s, Color c) {
            if (ttf) r.draw_text(*font_, x, y - 2, s, c);
            else r.draw_text(x, y, s, c);
        };
        auto text_width = [&](const char* s) {
            return ttf ? r.measure_text(*font_, s).x : 6.0f * static_cast<float>(std::strlen(s));
        };

        r.fill_rect(0, 0, kViewW, kMapTop, rgb(14, 16, 20));
        std::snprintf(buf, sizeof buf, "GOLD %d   LIVES %d   WAVE %d   SCORE %d", gold_, lives_, wave_, score_);
        text(4, 1, buf, rgb(255, 230, 140));
        if (!wave_active_) {
            std::snprintf(buf, sizeof buf, "next wave in %d   Enter: call early +%d gold", static_cast<int>(countdown_) + 1,
                          static_cast<int>(countdown_) * 3);
        } else {
            std::snprintf(buf, sizeof buf, "wave %d   %d left to spawn", wave_, to_spawn_);
        }
        text(kViewW - text_width(buf) - 4, 1, buf, rgb(200, 220, 255));

        // Bottom strip: tower selection on the left, wave status on the right.
        r.fill_rect(0, kMapBottom, kViewW, kViewH - kMapBottom, rgb(14, 16, 20));
        float x = 4;
        const float y = kMapBottom + 4;
        for (int i = 0; i < kTowerKinds; ++i) {
            const TowerSpec& s = kTowers[i];
            std::snprintf(buf, sizeof buf, "%s %s %d", i == selected_ ? ">" : " ", s.name, s.cost);
            const float w = text_width(buf) + 4;
            if (i == selected_) r.fill_rect(x - 2, y - 2, w, 12, rgba(255, 255, 255, 40));
            r.fill_rect(x, y + 10, w - 4, 1, s.color);
            text(x, y, buf, i == selected_ ? rgb(255, 255, 255) : s.color);
            x += w + 8;
        }
        text(x + 6, y, "X: next tower   left: build / upgrade   right: sell   F4: rewind", rgb(110, 120, 140));

        if (game_over_) {
            r.fill_rect(0, 0, kViewW, kViewH, rgba(0, 0, 0, 150));
            if (title_font_ && title_font_->ok) {
                const char* line1 = "The gate has fallen";
                const char* line2 = "Hold F4 to rewind time and try again";
                std::snprintf(buf, sizeof buf, "Wave %d   Score %d", wave_, score_);
                const float cx = kViewW * 0.5f;
                r.draw_text(*title_font_, cx - r.measure_text(*title_font_, line1).x * 0.5f, kViewH * 0.5f - 40, line1, rgb(255, 90, 90));
                r.draw_text(*font_, cx - r.measure_text(*font_, line2).x * 0.5f, kViewH * 0.5f + 4, line2, rgb(255, 255, 255));
                r.draw_text(*font_, cx - r.measure_text(*font_, buf).x * 0.5f, kViewH * 0.5f + 22, buf, rgb(255, 230, 140));
            } else {
                r.draw_text(kViewW * 0.5f - 27 * 6, kViewH * 0.5f - 24, "THE GATE HAS FALLEN", rgb(255, 90, 90), 3);
                r.draw_text(kViewW * 0.5f - 24 * 6, kViewH * 0.5f + 12, "HOLD F4 TO REWIND TIME AND TRY AGAIN", rgb(255, 255, 255), 2);
                std::snprintf(buf, sizeof buf, "WAVE %d   SCORE %d", wave_, score_);
                r.draw_text(kViewW * 0.5f - 9 * 6, kViewH * 0.5f + 34, buf, rgb(255, 230, 140), 2);
            }
        }
    }

    World* world() override { return &world_; }

    void debug_ui() override {
        ImGui::Text("enemies %zu  towers %zu  projectiles %zu  particles %zu", world_.count<Enemy>(),
                    world_.count<Tower>(), world_.count<Projectile>(), world_.count<Particle>());
        if (ImGui::Button("+100 gold")) gold_ += 100;
        ImGui::SameLine();
        if (ImGui::Button("next wave")) countdown_ = 0;
        ImGui::TextWrapped("Edits here bypass the simulation; the timeline records them as edits at this tick.");
    }

private:
    // ---- helpers -------------------------------------------------------------------

    static float tile_center_x(int tx) { return static_cast<float>(tx * kTile) + kTile * 0.5f; }
    static float tile_center_y(int ty) { return static_cast<float>(kMapTop + ty * kTile) + kTile * 0.5f; }

    static bool buildable(int tx, int ty) {
        if (tx < 0 || ty < 0 || tx >= kCols || ty >= kRows) return false;
        return kMap[static_cast<usize>(ty)][static_cast<usize>(tx)] == '.';
    }

    Entity tower_at(int tx, int ty) {
        Entity found = Entity::null();
        const float cx = tile_center_x(tx), cy = tile_center_y(ty);
        world_.each<Transform, Tower>([&](Entity e, Transform& t, Tower&) {
            if (absf(t.x - cx) < 1 && absf(t.y - cy) < 1) found = e;
        });
        return found;
    }

    static i32 upgrade_cost(const Tower& t) { return kTowers[t.kind].cost * t.level; }
    static float tower_range(const Tower& t) { return kTowers[t.kind].range * (1.0f + 0.12f * (t.level - 1)); }
    static float tower_damage(const Tower& t) { return kTowers[t.kind].damage * (1.0f + 0.6f * (t.level - 1)); }

    /// Walk the path from S to E and record every corner as a waypoint.
    void build_waypoints() {
        int sx = 0, sy = 0;
        for (int y = 0; y < kRows; ++y)
            for (int x = 0; x < kCols; ++x)
                if (kMap[static_cast<usize>(y)][static_cast<usize>(x)] == 'S') {
                    sx = x;
                    sy = y;
                }
        auto is_path = [](int x, int y) {
            if (x < 0 || y < 0 || x >= kCols || y >= kRows) return false;
            const char c = kMap[static_cast<usize>(y)][static_cast<usize>(x)];
            return c == '#' || c == 'S' || c == 'E';
        };
        // Start off-screen so enemies walk in through the gate.
        waypoints_.push_back(Vec2{tile_center_x(sx) - kTile, tile_center_y(sy)});
        int x = sx, y = sy, px = -1, py = -1;
        int dir_x = 0, dir_y = 0;
        for (int guard = 0; guard < kCols * kRows; ++guard) {
            const int dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
            int nx = -1, ny = -1;
            for (int d = 0; d < 4; ++d) {
                const int cx = x + dx[d], cy = y + dy[d];
                if ((cx == px && cy == py) || !is_path(cx, cy)) continue;
                nx = cx;
                ny = cy;
                break;
            }
            if (nx < 0) break;
            const int ndx = nx - x, ndy = ny - y;
            if (ndx != dir_x || ndy != dir_y) waypoints_.push_back(Vec2{tile_center_x(x), tile_center_y(y)});
            dir_x = ndx;
            dir_y = ndy;
            px = x;
            py = y;
            x = nx;
            y = ny;
            if (kMap[static_cast<usize>(y)][static_cast<usize>(x)] == 'E') break;
        }
        waypoints_.push_back(Vec2{tile_center_x(x), tile_center_y(y)});
        waypoints_.push_back(Vec2{tile_center_x(x) + kTile, tile_center_y(y)});
    }

    // Derived from the constant map; not state.
    std::vector<Vec2> waypoints_;

    // Assets.
    Font* font_ = nullptr;
    Font* title_font_ = nullptr;
    Sound* snd_shoot_ = nullptr;
    Sound* snd_hit_ = nullptr;
    Sound* snd_kill_ = nullptr;
    Sound* snd_leak_ = nullptr;
    Sound* snd_build_ = nullptr;
    Sound* snd_sell_ = nullptr;
    Sound* snd_wave_ = nullptr;
    Sound* snd_over_ = nullptr;
    Sound* snd_deny_ = nullptr;

    // Simulation state.
    World world_;
    i32 gold_ = 150;
    i32 lives_ = 20;
    i32 wave_ = 0;
    i32 score_ = 0;
    bool wave_active_ = false;
    float countdown_ = 8;
    i32 to_spawn_ = 0;
    u32 spawned_ = 0;
    float spawn_timer_ = 0;
    bool game_over_ = false;
    u8 selected_ = 0;
    float elapsed_ = 0;
    Rng rng_;
    InputFrame prev_;
};

REWIND_GAME(TowerDefense)
