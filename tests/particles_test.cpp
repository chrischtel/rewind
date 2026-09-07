// Headless checks for particles: bursts, ageing, gravity and drag,
// continuous emitters, capacity, determinism, colour and size over life.

#include <rewind/render/particles.h>

#include <cmath>
#include <cstdio>

using namespace rw;

namespace {

int failures = 0;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

ParticleDef spark() {
    ParticleDef d;
    d.id = "spark";
    d.lifetime_min = d.lifetime_max = 1.0f;
    d.speed_min = d.speed_max = 100;
    d.direction_deg = 0;
    d.spread_deg = 0;
    d.size_start = 10;
    d.size_end = 0;
    d.color_start = rgba(255, 0, 0, 255);
    d.color_end = rgba(0, 0, 255, 0);
    d.burst = 5;
    return d;
}

void test_burst_and_life() {
    const ParticleDef d = spark();
    ParticleSystem ps;
    ps.emit(d, {10, 20});
    CHECK(ps.count() == 5);
    ps.emit(d, {0, 0}, 3);
    CHECK(ps.count() == 8);
    // Straight along +x at 100 units per second.
    ps.update(0.5f);
    CHECK(ps.count() == 8);
    const LiveParticle& p = ps.particles()[0];
    CHECK(std::fabs(p.pos.x - 60) < 0.01f && std::fabs(p.pos.y - 20) < 0.01f);
    CHECK(std::fabs(p.size() - 5) < 0.01f); // halfway between 10 and 0
    const Color c = p.color();
    CHECK(c.r > 120 && c.r < 136 && c.b > 120 && c.b < 136 && c.a > 120 && c.a < 136);
    CHECK(&ps.def_of(p) == &d);
    ps.update(0.5f);
    CHECK(ps.count() == 0); // lifetime is exactly one second
}

void test_forces() {
    ParticleDef d = spark();
    d.gravity = {0, 100};
    d.speed_min = d.speed_max = 0;
    ParticleSystem ps;
    ps.emit(d, {0, 0}, 1);
    for (int i = 0; i < 10; ++i) ps.update(0.05f);
    const LiveParticle& p = ps.particles()[0];
    CHECK(std::fabs(p.vel.y - 50) < 0.01f); // 0.5 s of 100/s²
    CHECK(p.pos.y > 10 && p.pos.y < 15);    // explicit Euler lands a little above the exact 12.5

    ParticleDef slow = spark();
    slow.drag = 1;
    ParticleSystem ds;
    ds.emit(slow, {0, 0}, 1);
    ds.update(0.5f);
    CHECK(ds.particles()[0].vel.x < 100 && ds.particles()[0].vel.x > 40);

    ParticleDef spread = spark();
    spread.spread_deg = 360;
    spread.radius = 5;
    ParticleSystem ss;
    ss.emit(spread, {0, 0}, 50);
    bool up = false, down = false, offset = false;
    for (const LiveParticle& q : ss.particles()) {
        if (q.vel.y < -10) up = true;
        if (q.vel.y > 10) down = true;
        if (std::fabs(q.pos.x) > 0.5f || std::fabs(q.pos.y) > 0.5f) offset = true;
        CHECK(std::fabs(std::sqrt(q.pos.x * q.pos.x + q.pos.y * q.pos.y)) <= 5.001f);
    }
    CHECK(up && down && offset);
}

void test_emitters_and_capacity() {
    ParticleDef d = spark();
    d.emit_rate = 100;
    d.lifetime_min = d.lifetime_max = 10;
    ParticleSystem ps;
    const u32 e = ps.add_emitter(d, {0, 0});
    CHECK(ps.has_emitter(e) && ps.emitter_count() == 1);
    for (int i = 0; i < 60; ++i) ps.update(1.0f / 60);
    CHECK(ps.count() >= 99 && ps.count() <= 101);
    ps.move_emitter(e, {500, 0});
    ps.update(0.02f);
    CHECK(ps.particles().back().pos.x > 400);
    ps.remove_emitter(e);
    CHECK(!ps.has_emitter(e) && ps.emitter_count() == 0);
    const usize n = ps.count();
    ps.update(0.1f);
    CHECK(ps.count() == n);

    ps.set_capacity(50);
    ps.emit(d, {0, 0}, 30);
    CHECK(ps.count() == 50); // the oldest made room
    ps.clear();
    CHECK(ps.count() == 0);
}

void test_determinism() {
    ParticleDef d = spark();
    d.spread_deg = 360;
    d.lifetime_min = 0.5f;
    d.lifetime_max = 2;
    ParticleSystem a(7), b(7), c(8);
    a.emit(d, {0, 0}, 20);
    b.emit(d, {0, 0}, 20);
    c.emit(d, {0, 0}, 20);
    a.update(0.1f);
    b.update(0.1f);
    c.update(0.1f);
    bool same = true, differs = false;
    for (usize i = 0; i < 20; ++i) {
        same = same && a.particles()[i].pos.x == b.particles()[i].pos.x && a.particles()[i].lifetime == b.particles()[i].lifetime;
        differs = differs || a.particles()[i].pos.x != c.particles()[i].pos.x;
    }
    CHECK(same && differs);
}

} // namespace

int main() {
    test_burst_and_life();
    test_forces();
    test_emitters_and_capacity();
    test_determinism();
    if (failures == 0) std::printf("particles_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
