#pragma once

// Particles: sparks, smoke, dust, rain. A ParticleDef says what a particle
// is (lifetime, speed, spread, gravity, drag, size and colour over life,
// spin, texture); a ParticleSystem holds the live ones, emits bursts and
// runs continuous emitters, integrates them and draws them.
//
// Particles are presentation: update them in render() with the frame's dt
// and they never touch the simulation, the fingerprint or the timeline.
// Emit them from events (on_event) so a rewind and replay re-fires them.
// The random numbers come from the system's own seeded Rng, so the same
// calls in the same order give the same picture (screenshots, tests).
//
// A ParticleDef is a reflected struct with an id: keep them in a DataTable
// (project/data_table.h) and designers tune them in the editor's Data
// panel while the game runs.

#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/project/asset_ref.h"
#include "rewind/render/renderer2d.h"
#include "rewind/sim/reflect.h"
#include "rewind/sim/rng.h"

#include <functional>
#include <string>
#include <vector>

namespace rw {

struct ParticleDef {
    std::string id;
    float lifetime_min = 0.4f;
    float lifetime_max = 0.8f;
    float speed_min = 40;
    float speed_max = 120;
    float direction_deg = -90; ///< 0 = +x, 90 = +y (down in screen space), -90 = up
    float spread_deg = 360;    ///< cone around the direction; 360 = every direction
    float radius = 0;          ///< spawn anywhere within this distance of the emit point
    Vec2 gravity{0, 0};        ///< units per second squared
    float drag = 0;            ///< fraction of the velocity lost per second
    float size_start = 6;
    float size_end = 0;
    Color color_start = rgb(255, 255, 255);
    Color color_end = rgba(255, 255, 255, 0);
    float spin_deg = 0;        ///< rotation speed, random sign; 0 = no rotation
    float emit_rate = 0;       ///< continuous emitters: particles per second
    u32 burst = 12;            ///< emit() count when none is given
    AssetRef texture;          ///< optional; untextured particles are discs

    void reflect(Visitor& v) {
        v.field("id", id);
        v.field("lifetime_min", lifetime_min, {.min = 0.01f, .max = 60, .speed = 0.01f});
        v.field("lifetime_max", lifetime_max, {.min = 0.01f, .max = 60, .speed = 0.01f});
        v.field("speed_min", speed_min, {.min = 0, .max = 5000});
        v.field("speed_max", speed_max, {.min = 0, .max = 5000});
        v.field("direction_deg", direction_deg, {.min = -360, .max = 360});
        v.field("spread_deg", spread_deg, {.min = 0, .max = 360});
        v.field("radius", radius, {.min = 0, .max = 1000});
        v.field("gravity", gravity);
        v.field("drag", drag, {.min = 0, .max = 20, .speed = 0.05f});
        v.field("size_start", size_start, {.min = 0, .max = 512});
        v.field("size_end", size_end, {.min = 0, .max = 512});
        v.field("color_start", color_start);
        v.field("color_end", color_end);
        v.field("spin_deg", spin_deg, {.min = 0, .max = 3600});
        v.field("emit_rate", emit_rate, {.min = 0, .max = 10000});
        v.field("burst", burst, {.min = 0, .max = 10000});
        v.field("texture", texture, {.asset_type = "texture"});
    }
};

struct LiveParticle {
    Vec2 pos;
    Vec2 vel;
    Vec2 gravity;
    float drag = 0;
    float age = 0;
    float lifetime = 1;
    float size0 = 1;
    float size1 = 0;
    float rotation = 0; ///< degrees
    float spin = 0;     ///< degrees per second
    Color c0;
    Color c1;
    u32 def = 0; ///< index into the system's def list (for the texture)

    float t() const { return lifetime > 0 ? age / lifetime : 1; }
    float size() const { return size0 + (size1 - size0) * t(); }
    Color color() const;
};

class ParticleSystem {
public:
    explicit ParticleSystem(u32 seed = 1) : rng_(seed) {}

    void set_seed(u32 seed) { rng_ = Rng(seed); }
    /// Oldest particles make room when full. Default 4096.
    void set_capacity(usize max) {
        capacity_ = max < 1 ? 1 : max;
        if (particles_.size() > capacity_) particles_.erase(particles_.begin(), particles_.begin() + static_cast<std::ptrdiff_t>(particles_.size() - capacity_));
    }
    usize capacity() const { return capacity_; }

    /// A burst at `at` (count 0 = def.burst) with an extra velocity added
    /// to every particle (a moving source). The def must stay alive while
    /// its particles do; it is only read again to pick a texture.
    void emit(const ParticleDef& def, Vec2 at, u32 count = 0, Vec2 velocity = {});

    /// A continuous emitter at def.emit_rate per second. Returns its handle.
    u32 add_emitter(const ParticleDef& def, Vec2 at);
    void move_emitter(u32 emitter, Vec2 at);
    void remove_emitter(u32 emitter);
    bool has_emitter(u32 emitter) const;
    usize emitter_count() const { return emitters_.size(); }

    /// Integrate and age everything; dead particles are dropped.
    void update(float dt);

    /// Discs of the particle's size and colour.
    void draw(Renderer2D& r) const;
    /// Textured: `texture_of` maps a def to its texture (null = disc).
    void draw(Renderer2D& r, const std::function<const Texture*(const ParticleDef&)>& texture_of) const;

    usize count() const { return particles_.size(); }
    const std::vector<LiveParticle>& particles() const { return particles_; }
    const ParticleDef& def_of(const LiveParticle& p) const { return *defs_[p.def]; }
    void clear();

private:
    struct Emitter {
        u32 id;
        const ParticleDef* def;
        Vec2 at;
        float accumulator;
    };
    u32 def_index(const ParticleDef& def);
    void spawn(const ParticleDef& def, u32 def_index, Vec2 at, Vec2 velocity);

    std::vector<LiveParticle> particles_;
    std::vector<const ParticleDef*> defs_;
    std::vector<Emitter> emitters_;
    Rng rng_;
    usize capacity_ = 4096;
    u32 next_emitter_ = 1;
};

} // namespace rw
