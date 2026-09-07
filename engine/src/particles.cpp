#include "rewind/render/particles.h"

#include "rewind/render/renderer2d.h"

#include <algorithm>
#include <cmath>

namespace rw {

namespace {
constexpr float kDegToRad = 3.14159265f / 180.0f;

u8 lerp_u8(u8 a, u8 b, float t) { return static_cast<u8>(a + (static_cast<float>(b) - static_cast<float>(a)) * t + 0.5f); }
} // namespace

Color LiveParticle::color() const {
    const float k = t();
    return Color{lerp_u8(c0.r, c1.r, k), lerp_u8(c0.g, c1.g, k), lerp_u8(c0.b, c1.b, k), lerp_u8(c0.a, c1.a, k)};
}

u32 ParticleSystem::def_index(const ParticleDef& def) {
    for (usize i = 0; i < defs_.size(); ++i)
        if (defs_[i] == &def) return static_cast<u32>(i);
    defs_.push_back(&def);
    return static_cast<u32>(defs_.size() - 1);
}

void ParticleSystem::spawn(const ParticleDef& def, u32 def_index, Vec2 at, Vec2 velocity) {
    if (particles_.size() >= capacity_) particles_.erase(particles_.begin());
    LiveParticle p;
    const float angle = (def.direction_deg + rng_.range_f(-def.spread_deg * 0.5f, def.spread_deg * 0.5f)) * kDegToRad;
    const float speed = rng_.range_f(def.speed_min, def.speed_max);
    p.pos = at;
    if (def.radius > 0) {
        const float a = rng_.range_f(0, 6.2831853f);
        const float d = std::sqrt(rng_.range_f(0, 1)) * def.radius; // even over the disc
        p.pos = {at.x + std::cos(a) * d, at.y + std::sin(a) * d};
    }
    p.vel = {std::cos(angle) * speed + velocity.x, std::sin(angle) * speed + velocity.y};
    p.gravity = def.gravity;
    p.drag = def.drag;
    p.age = 0;
    p.lifetime = std::max(0.001f, rng_.range_f(def.lifetime_min, def.lifetime_max));
    p.size0 = def.size_start;
    p.size1 = def.size_end;
    p.rotation = def.spin_deg > 0 ? rng_.range_f(0, 360) : 0;
    p.spin = def.spin_deg > 0 ? rng_.range_f(-def.spin_deg, def.spin_deg) : 0;
    p.c0 = def.color_start;
    p.c1 = def.color_end;
    p.def = def_index;
    particles_.push_back(p);
}

void ParticleSystem::emit(const ParticleDef& def, Vec2 at, u32 count, Vec2 velocity) {
    const u32 n = count ? count : def.burst;
    if (n == 0) return;
    const u32 index = def_index(def);
    for (u32 i = 0; i < n; ++i) spawn(def, index, at, velocity);
}

u32 ParticleSystem::add_emitter(const ParticleDef& def, Vec2 at) {
    def_index(def);
    emitters_.push_back(Emitter{next_emitter_, &def, at, 0});
    return next_emitter_++;
}

void ParticleSystem::move_emitter(u32 emitter, Vec2 at) {
    for (Emitter& e : emitters_)
        if (e.id == emitter) e.at = at;
}

void ParticleSystem::remove_emitter(u32 emitter) {
    emitters_.erase(std::remove_if(emitters_.begin(), emitters_.end(), [&](const Emitter& e) { return e.id == emitter; }),
                    emitters_.end());
}

bool ParticleSystem::has_emitter(u32 emitter) const {
    for (const Emitter& e : emitters_)
        if (e.id == emitter) return true;
    return false;
}

void ParticleSystem::update(float dt) {
    if (dt <= 0) return;
    for (Emitter& e : emitters_) {
        if (e.def->emit_rate <= 0) continue;
        e.accumulator += e.def->emit_rate * dt;
        const u32 index = def_index(*e.def);
        while (e.accumulator >= 1) {
            spawn(*e.def, index, e.at, {});
            e.accumulator -= 1;
        }
    }
    for (LiveParticle& p : particles_) {
        p.age += dt;
        p.vel.x += p.gravity.x * dt;
        p.vel.y += p.gravity.y * dt;
        if (p.drag > 0) {
            const float k = std::max(0.0f, 1 - p.drag * dt);
            p.vel.x *= k;
            p.vel.y *= k;
        }
        p.pos.x += p.vel.x * dt;
        p.pos.y += p.vel.y * dt;
        p.rotation += p.spin * dt;
    }
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(), [](const LiveParticle& p) { return p.age >= p.lifetime; }),
                     particles_.end());
}

void ParticleSystem::draw(Renderer2D& r) const {
    for (const LiveParticle& p : particles_) {
        const float size = p.size();
        if (size <= 0) continue;
        r.fill_circle(p.pos.x, p.pos.y, size * 0.5f, p.color(), 8);
    }
}

void ParticleSystem::draw(Renderer2D& r, const std::function<const Texture*(const ParticleDef&)>& texture_of) const {
    for (const LiveParticle& p : particles_) {
        const float size = p.size();
        if (size <= 0) continue;
        const Texture* tex = texture_of ? texture_of(*defs_[p.def]) : nullptr;
        if (!tex || !tex->handle || tex->width <= 0) {
            r.fill_circle(p.pos.x, p.pos.y, size * 0.5f, p.color(), 8);
            continue;
        }
        const Rect src{0, 0, static_cast<float>(tex->width), static_cast<float>(tex->height)};
        SpriteOptions o;
        o.scale_x = o.scale_y = size / src.w;
        o.rotation_deg = p.rotation;
        o.origin = {src.w * 0.5f, src.h * 0.5f};
        o.tint = p.color();
        r.draw_sprite(*tex, src, p.pos.x, p.pos.y, o);
    }
}

void ParticleSystem::clear() {
    particles_.clear();
    emitters_.clear();
}

} // namespace rw
