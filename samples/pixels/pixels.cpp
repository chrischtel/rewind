// Pixels: a 320x180 pixel-art platformer scene that shows sprites, animation,
// tilemap collision, sound through simulation events, the integer-scaled
// render target, and gamepad input. Every asset is generated in code so the
// repository ships no binary files; a real game would call
// load_aseprite_sheet() and load_aseprite_tilemap() in load_assets() instead.
//
// Run and jump with the keyboard or a gamepad, then hold F4 to rewind. The
// animation state is part of the saved state, so it rewinds too, and the
// jump and landing sounds never replay during a rewind or a replay because
// they are triggered through emit() and only heard for live ticks.

#include <rewind/rewind.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace rw;

namespace {

constexpr int kViewW = 320;
constexpr int kViewH = 180;
constexpr int kHero = 16; // sprite size in pixels
constexpr int kTile = 16;

// Events the simulation can emit. Their meaning lives in on_event().
enum Event : u32 { kEvJump = 1, kEvLand = 2, kEvBump = 3 };

struct Star {
    float x, y;
    u8 depth; ///< 1 far, 3 near

    void reflect(Visitor& v) {
        v.field("x", x);
        v.field("y", y);
        v.field("depth", depth, {.min = 1, .max = 3});
    }
};

/// Tiny pixel canvas for building textures in code.
struct Canvas {
    int w, h;
    std::vector<u8> px;
    Canvas(int width, int height) : w(width), h(height), px(static_cast<usize>(width) * height * 4, 0) {}

    void put(int x, int y, Color c) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        u8* p = &px[(static_cast<usize>(y) * w + x) * 4];
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
        p[3] = c.a;
    }
    void rect(int x, int y, int rw, int rh, Color c) {
        for (int j = 0; j < rh; ++j)
            for (int i = 0; i < rw; ++i) put(x + i, y + j, c);
    }
};

// Level: '#' ground, '=' platform, '^' spikes-ish decoration (not solid).
const std::vector<std::string> kLevel = {
    "................................................................................",
    "................................................................................",
    "................................................................................",
    "..............................................=====.............................",
    "...........................................................====.................",
    "......................=====.....................................................",
    "..................................====................................====......",
    "............====................................................................",
    "..........................................=====.................................",
    "................................................................................",
    "###############......########################.....##############################",
    "###############......########################.....##############################",
};

/// A short synthesized sound: square-ish blip with a pitch sweep and decay.
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

} // namespace

class Pixels final : public Game {
public:
    void configure(AppConfig& cfg) override {
        cfg.title = "Rewind - pixels";
        cfg.width = 1280;
        cfg.height = 720;
        cfg.render_width = kViewW;
        cfg.render_height = kViewH;
        cfg.integer_scale = true;
        cfg.pixel_art = true;
        cfg.timeline.history_ticks = 60 * 120;
    }

    void load_assets(Renderer2D& r, Audio& audio) override {
        // Hero sheet: 6 frames of 16x16. 0 idle, 1-4 walk, 5 jump.
        Canvas hero(kHero * 6, kHero);
        const Color body = rgb(235, 235, 245), shade = rgb(150, 160, 200), eye = rgb(30, 30, 60),
                    visor = rgb(90, 220, 255), boot = rgb(60, 60, 90);
        const int leg_offsets[6][2] = {{0, 0}, {-2, 1}, {0, 0}, {2, -1}, {0, 0}, {-1, -1}};
        for (int f = 0; f < 6; ++f) {
            const int ox = f * kHero;
            const int bob = (f == 2 || f == 4) ? 1 : 0;
            const int lift = f == 5 ? -2 : 0;
            hero.rect(ox + 4, 1 + bob + lift, 8, 6, body);
            hero.rect(ox + 5, 3 + bob + lift, 6, 2, visor);
            hero.put(ox + 6, 3 + bob + lift, eye);
            hero.put(ox + 9, 3 + bob + lift, eye);
            hero.rect(ox + 3, 7 + bob + lift, 10, 5, body);
            hero.rect(ox + 3, 11 + bob + lift, 10, 1, shade);
            hero.rect(ox + 1, 8 + bob + lift, 2, 3, shade);
            hero.rect(ox + 13, 8 + bob + lift, 2, 3, shade);
            const int l = leg_offsets[f][0], rr = leg_offsets[f][1];
            hero.rect(ox + 4 + l, 12 + lift, 3, 4, boot);
            hero.rect(ox + 9 + rr, 12 + lift, 3, 4, boot);
        }
        Texture* hero_tex = r.create_texture("hero", hero.w, hero.h, hero.px.data());
        sheet_ = sprite_sheet_from_grid(hero_tex, kHero, kHero, 6, 90);
        sheet_.tags = {
            SpriteTag{"idle", 0, 0, TagDirection::Forward, 0},
            SpriteTag{"walk", 1, 4, TagDirection::Forward, 0},
            SpriteTag{"jump", 5, 5, TagDirection::Forward, 0},
        };
        sheet_.slices = {SpriteSlice{"hitbox", Rect{3, 1, 10, 15}, false, {}}};

        // Tileset, stacked vertically: 0 empty, 1 ground, 2 platform.
        Canvas tiles(kTile, kTile * 3);
        tiles.rect(0, 16, kTile, kTile, rgb(70, 50, 60));
        tiles.rect(0, 16, kTile, 3, rgb(120, 190, 90));
        tiles.rect(0, 19, kTile, 1, rgb(90, 140, 70));
        for (int i = 0; i < 5; ++i) tiles.put((i * 7) % kTile, 22 + (i * 5) % 9, rgb(90, 65, 75));
        tiles.rect(0, 32, kTile, 5, rgb(180, 140, 90));
        tiles.rect(0, 32, kTile, 1, rgb(230, 200, 140));
        tiles.rect(1, 37, 2, 3, rgb(120, 90, 60));
        tiles.rect(13, 37, 2, 3, rgb(120, 90, 60));
        Texture* tileset = r.create_texture("tiles", tiles.w, tiles.h, tiles.px.data());

        map_ = tilemap_from_text(kLevel, "#=", kTile, kTile);
        map_.tileset = tileset;
        map_.tileset_columns = 1;

        // Moon.
        Canvas moon(12, 12);
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 12; ++x) {
                const float dx = x - 5.5f, dy = y - 5.5f;
                if (dx * dx + dy * dy < 30) moon.put(x, y, rgb(240, 230, 200));
            }
        moon.rect(3, 3, 2, 2, rgb(210, 200, 170));
        moon.rect(7, 6, 3, 2, rgb(210, 200, 170));
        moon_ = r.create_texture("moon", moon.w, moon.h, moon.px.data());

        // Palette swap material: a GLSL fragment shader compiled by the build
        // (see CMakeLists SHADERS) plus a 4x1 palette texture per look.
        palette_material_ = r.load_material("shaders/palette.frag.spv");
        const Color palettes[kPaletteCount][4] = {
            {rgb(40, 20, 60), rgb(120, 60, 140), rgb(220, 120, 200), rgb(255, 230, 250)},   // dusk
            {rgb(10, 40, 30), rgb(30, 120, 80), rgb(90, 220, 120), rgb(230, 255, 200)},     // toxic
            {rgb(60, 20, 10), rgb(180, 60, 20), rgb(255, 160, 40), rgb(255, 240, 180)},     // ember
        };
        for (int p = 0; p < kPaletteCount; ++p) {
            Canvas strip(4, 1);
            for (int i = 0; i < 4; ++i) strip.put(i, 0, palettes[p][i]);
            palette_tex_[p] = r.create_texture("palette" + std::to_string(p), 4, 1, strip.px.data());
        }

        // Sounds, synthesized so the repository stays free of binary assets.
        const std::vector<float> jump = synth(0.14f, 320, 760, 0.05f, 1);
        const std::vector<float> land = synth(0.08f, 140, 60, 0.6f, 2);
        const std::vector<float> bump = synth(0.05f, 200, 120, 0.3f, 3);
        snd_jump_ = audio.create_sound("jump", 48000, 1, jump.data(), jump.size());
        snd_land_ = audio.create_sound("land", 48000, 1, land.data(), land.size());
        snd_bump_ = audio.create_sound("bump", 48000, 1, bump.data(), bump.size());
    }

    void init() override {
        x_ = 40;
        y_ = 10 * kTile - kHero;
        vx_ = vy_ = 0;
        on_ground_ = true;
        facing_left_ = false;
        jumps_ = 0;
        rng_ = Rng(99);
        stars_.clear();
        for (int i = 0; i < 90; ++i) {
            stars_.push_back(Star{rng_.range_f(0, map_.pixel_width()), rng_.range_f(0, 130),
                                  static_cast<u8>(rng_.range(1, 3))});
        }
        anim_ = AnimState{};
        anim_play(anim_, sheet_, "idle");
        palette_ = 1; // start with the "dusk" palette so the material is visible right away
    }

    void tick(const TickContext& c) override {
        RW_PROFILE_SCOPE("pixels.tick");
        const float dt = c.dt;
        const InputFrame& in = c.input;
        const float move = static_cast<float>(in.axis_x) / 32767.0f;

        // Horizontal: snappy acceleration, friction when idle.
        const float target = move * 110.0f;
        const float accel = on_ground_ ? 900.0f : 500.0f;
        if (target > vx_) vx_ = std::min(vx_ + accel * dt, target);
        else vx_ = std::max(vx_ - accel * dt, target);
        if (move > 0.05f) facing_left_ = false;
        if (move < -0.05f) facing_left_ = true;

        // Jump on the rising edge of A, with a small hop if released early.
        if (pressed(in, prev_input_, Button::A) && on_ground_) {
            vy_ = -200.0f;
            on_ground_ = false;
            ++jumps_;
            c.emit(kEvJump);
        }
        if (pressed(in, prev_input_, Button::B)) palette_ = (palette_ + 1) % (kPaletteCount + 1); // 0 = off
        if (!in.down(Button::A) && vy_ < -60.0f) vy_ = -60.0f;
        prev_input_ = in;

        vy_ = std::min(vy_ + 560.0f * dt, 300.0f);

        // Move the hitbox through the tilemap; the sprite hangs off it.
        const Rect hb = hitbox();
        Rect box{x_ + hb.x, y_ + hb.y, hb.w, hb.h};
        const TileHit hit = tilemap_move(map_, box, vx_ * dt, vy_ * dt);
        x_ = box.x - hb.x;
        y_ = box.y - hb.y;

        if (hit.left || hit.right) {
            if (absf(vx_) > 60.0f) c.emit(kEvBump, absf(vx_) / 110.0f);
            vx_ = 0;
        }
        if (hit.top) vy_ = 0;
        const bool was_airborne = !on_ground_;
        if (hit.bottom) {
            if (was_airborne && vy_ > 120.0f) c.emit(kEvLand, std::min(vy_ / 300.0f, 1.0f));
            vy_ = 0;
            on_ground_ = true;
        } else {
            on_ground_ = false;
        }

        // Fell out of the world: back to the start.
        if (y_ > map_.pixel_height() + 40) {
            x_ = 40;
            y_ = 10 * kTile - kHero;
            vx_ = vy_ = 0;
        }

        // Pick the animation from the state, never the other way round.
        if (!on_ground_) anim_play(anim_, sheet_, "jump");
        else if (absf(vx_) > 8.0f) anim_play(anim_, sheet_, "walk");
        else anim_play(anim_, sheet_, "idle");
        anim_advance(anim_, sheet_, dt);
    }

    void on_event(const SimEvent& e, Audio& audio) override {
        switch (e.id) {
        case kEvJump: audio.play(snd_jump_, 0.5f); break;
        case kEvLand: audio.play(snd_land_, 0.4f + 0.4f * e.a); break;
        case kEvBump: audio.play(snd_bump_, 0.3f * e.a); break;
        default: break;
        }
    }

    void reflect(Visitor& v) override {
        v.field("x", x_, {.speed = 0.5f});
        v.field("y", y_, {.speed = 0.5f});
        v.field("vx", vx_);
        v.field("vy", vy_);
        v.field("on_ground", on_ground_);
        v.field("facing_left", facing_left_);
        v.field("jumps", jumps_);
        v.field("prev_input", prev_input_);
        v.field("anim", anim_);
        v.field("rng", rng_);
        v.field("stars", stars_);
        v.field("palette", palette_, {.min = 0, .max = kPaletteCount});
        v.field("palette_mix", palette_mix_, {.min = 0, .max = 1, .transient = true});
    }

    void render(Renderer2D& r, float /*alpha*/) override {
        r.clear(rgb(16, 14, 40));

        // Camera follows the hero, clamped to the level. Render-side only.
        const float cam_x = clampf(x_ + kHero * 0.5f - kViewW * 0.5f, 0, map_.pixel_width() - kViewW);
        const float cam_y = clampf(map_.pixel_height() - kViewH, 0, map_.pixel_height());
        r.set_camera(Camera{std::floor(cam_x), std::floor(cam_y), 1});

        // Parallax sky: stars scroll slower the farther away they are.
        const float world_w = map_.pixel_width();
        for (const Star& s : stars_) {
            const float factor = 0.15f * s.depth;
            float sx = s.x - cam_x * factor;
            sx = sx - world_w * std::floor(sx / world_w); // wrap
            const Color c = s.depth == 3 ? rgb(255, 255, 255)
                                         : (s.depth == 2 ? rgb(170, 170, 200) : rgb(100, 100, 140));
            r.fill_rect(std::floor(cam_x + sx), s.y, 1, 1, c);
        }
        if (moon_) r.draw_texture(*moon_, std::floor(cam_x) + 250, 20);

        draw_tilemap(r, map_);

        // Hero, through the palette material when a palette is selected.
        if (sheet_.valid()) {
            SpriteOptions o;
            o.flip_x = facing_left_;
            const bool swapped = palette_ > 0 && palette_material_ && palette_material_->ok;
            if (swapped) {
                struct Params {
                    float mix_amount;
                    float steps;
                } params{palette_mix_, 4.0f};
                r.set_material(palette_material_);
                r.set_material_params(params);
                r.set_material_texture(1, palette_tex_[palette_ - 1]);
            }
            r.draw_sprite(*sheet_.texture, anim_frame(anim_, sheet_).src, std::floor(x_), std::floor(y_), o);
            if (swapped) r.set_material(nullptr);
            if (show_hitbox_) {
                const Rect hb = hitbox();
                r.draw_rect(std::floor(x_) + hb.x, std::floor(y_) + hb.y, hb.w, hb.h, rgba(255, 80, 80, 200));
            }
        }

        r.set_camera(Camera{});
        r.draw_text(4, 4, "stick/arrows: run  Z/A: jump", rgb(200, 200, 220));
        r.draw_text(4, 14, "X/B: palette   hold F4: rewind", rgb(140, 140, 170));
        if (palette_material_ && !palette_material_->ok) r.draw_text(4, 24, "palette shader failed (see log)", rgb(255, 90, 90));
        char buf[64];
        std::snprintf(buf, sizeof buf, "jumps %u", jumps_);
        r.draw_text(4, kViewH - 12, buf, rgb(140, 140, 170));
    }

    void debug_ui() override {
        ImGui::Text("pos %.1f, %.1f   vel %.0f, %.0f   %s", x_, y_, vx_, vy_,
                    on_ground_ ? "grounded" : "airborne");
        ImGui::Text("anim tag %u frame %u (%u us)", anim_.tag, anim_.frame, anim_.elapsed_us);
        ImGui::Checkbox("show hitbox slice", &show_hitbox_);
        ImGui::SliderFloat("palette mix", &palette_mix_, 0.0f, 1.0f);
        if (palette_material_) {
            ImGui::Text("material build #%u %s", palette_material_->generation,
                        palette_material_->ok ? "ok" : palette_material_->error.c_str());
            ImGui::TextWrapped("edit samples/pixels/shaders/palette.frag and save: it recompiles live.");
        }
    }

private:
    Rect hitbox() const {
        const SpriteSlice* s = sheet_.find_slice("hitbox");
        Rect hb = s ? s->bounds : Rect{0, 0, kHero, kHero};
        if (facing_left_) hb.x = kHero - hb.x - hb.w;
        return hb;
    }

    static constexpr int kPaletteCount = 3;

    // Assets and level (rebuilt in load_assets, never part of the saved state).
    SpriteSheet sheet_;
    TileMap map_;
    Texture* moon_ = nullptr;
    Material* palette_material_ = nullptr;
    Texture* palette_tex_[kPaletteCount] = {};
    float palette_mix_ = 1.0f;
    Sound* snd_jump_ = nullptr;
    Sound* snd_land_ = nullptr;
    Sound* snd_bump_ = nullptr;
    bool show_hitbox_ = false;

    // Simulation state.
    float x_ = 0, y_ = 0, vx_ = 0, vy_ = 0;
    bool on_ground_ = true;
    bool facing_left_ = false;
    u32 jumps_ = 0;
    InputFrame prev_input_;
    AnimState anim_;
    Rng rng_;
    std::vector<Star> stars_;
    i32 palette_ = 0; ///< 0 = original colours, else palette index + 1
};

REWIND_GAME(Pixels)
