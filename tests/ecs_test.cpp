// Headless checks for the World: entity lifetime, components, iteration,
// reflection round trips, determinism, and the JSON text format.

#include <rewind/sim/reflect.h>
#include <rewind/sim/serialize.h>
#include <rewind/sim/text_format.h>
#include <rewind/sim/world.h>

#include <cstdio>
#include <string>
#include <vector>

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

struct Pos {
    float x = 0, y = 0;
    void reflect(Visitor& v) {
        v.field("x", x);
        v.field("y", y);
    }
};

struct Vel {
    float dx = 0, dy = 0;
    void reflect(Visitor& v) {
        v.field("dx", dx);
        v.field("dy", dy);
    }
};

struct Tag {
    std::string name;
    Entity target;
    void reflect(Visitor& v) {
        v.field("name", name);
        v.field("target", target);
    }
};

World fresh_world() {
    World w;
    w.register_component<Pos>("Pos");
    w.register_component<Vel>("Vel");
    w.register_component<Tag>("Tag");
    return w;
}

u64 fingerprint(World& w) {
    Writer out;
    save_reflected(w, out);
    return hash_bytes(out.bytes());
}

void test_entity_lifetime() {
    World w = fresh_world();
    const Entity a = w.create();
    const Entity b = w.create();
    CHECK(w.alive(a) && w.alive(b));
    CHECK(a != b);
    CHECK(w.entity_count() == 2);
    CHECK(!w.alive(Entity::null()));

    w.destroy(a);
    CHECK(!w.alive(a));
    CHECK(w.entity_count() == 1);

    // The slot is reused with a new generation: the old handle stays dead.
    const Entity c = w.create();
    CHECK(c.index == a.index);
    CHECK(c.generation != a.generation);
    CHECK(w.alive(c) && !w.alive(a));

    w.destroy(a); // double destroy through a stale handle is a no-op
    CHECK(w.alive(c));
    CHECK(w.entity_count() == 2);
}

void test_components() {
    World w = fresh_world();
    const Entity e = w.create();
    CHECK(!w.has<Pos>(e));
    CHECK(w.get<Pos>(e) == nullptr);

    w.add(e, Pos{1, 2});
    CHECK(w.has<Pos>(e));
    CHECK(w.get<Pos>(e)->x == 1);
    w.get<Pos>(e)->x = 5;
    CHECK(w.get<Pos>(e)->x == 5);

    w.add(e, Pos{9, 9}); // replace
    CHECK(w.get<Pos>(e)->x == 9);
    CHECK(w.count<Pos>() == 1);

    w.remove<Pos>(e);
    CHECK(!w.has<Pos>(e));
    CHECK(w.count<Pos>() == 0);

    w.add(e, Vel{1, 1});
    w.destroy(e);
    CHECK(w.count<Vel>() == 0); // destroy strips every component
}

void test_each() {
    World w = fresh_world();
    std::vector<Entity> ents;
    for (int i = 0; i < 10; ++i) {
        const Entity e = w.create();
        w.add(e, Pos{static_cast<float>(i), 0});
        if (i % 2 == 0) w.add(e, Vel{1, 0});
        ents.push_back(e);
    }

    int moving = 0;
    w.each<Pos, Vel>([&](Entity, Pos& p, Vel& v) {
        p.x += v.dx;
        ++moving;
    });
    CHECK(moving == 5);
    CHECK(w.get<Pos>(ents[0])->x == 1);
    CHECK(w.get<Pos>(ents[1])->x == 1); // untouched: no Vel
    CHECK(w.get<Pos>(ents[2])->x == 3);

    // Swap-remove keeps the others intact.
    w.destroy(ents[2]);
    int seen = 0;
    w.each<Pos>([&](Entity e, Pos&) {
        CHECK(w.alive(e));
        ++seen;
    });
    CHECK(seen == 9);
    CHECK(w.get<Pos>(ents[9])->x == 9);
}

void test_reflect_round_trip() {
    World a = fresh_world();
    const Entity e1 = a.create();
    const Entity e2 = a.create();
    const Entity e3 = a.create();
    a.add(e1, Pos{1, 1});
    a.add(e2, Pos{2, 2});
    a.add(e2, Vel{3, 3});
    a.add(e3, Tag{"chaser", e2});
    a.destroy(e1); // leaves a hole in the slots and a free entry

    Writer out;
    save_reflected(a, out);

    World b = fresh_world();
    Reader in(out.bytes());
    load_reflected(b, in);
    CHECK(in.ok() && in.remaining() == 0);

    CHECK(b.entity_count() == 2);
    CHECK(!b.alive(e1));
    CHECK(b.alive(e2) && b.alive(e3));
    CHECK(b.get<Pos>(e2) && b.get<Pos>(e2)->x == 2);
    CHECK(b.get<Vel>(e2) && b.get<Vel>(e2)->dy == 3);
    CHECK(b.get<Tag>(e3) && b.get<Tag>(e3)->name == "chaser");
    CHECK(b.get<Tag>(e3)->target == e2);
    CHECK(b.alive(b.get<Tag>(e3)->target));

    // Slot reuse after load follows the same free list.
    const Entity ra = a.create();
    const Entity rb = b.create();
    CHECK(ra == rb);
    CHECK(fingerprint(a) == fingerprint(b));
}

void test_determinism() {
    World a = fresh_world();
    World b = fresh_world();
    auto script = [](World& w) {
        std::vector<Entity> es;
        for (int i = 0; i < 50; ++i) {
            const Entity e = w.create();
            w.add(e, Pos{static_cast<float>(i), static_cast<float>(i * 2)});
            if (i % 3 == 0) w.add(e, Vel{1, 2});
            es.push_back(e);
        }
        for (int i = 0; i < 50; i += 7) w.destroy(es[static_cast<usize>(i)]);
        for (int i = 0; i < 5; ++i) w.add(w.create(), Pos{-1, -1});
        w.each<Pos, Vel>([](Entity, Pos& p, Vel& v) { p.x += v.dx; });
    };
    script(a);
    script(b);
    CHECK(fingerprint(a) == fingerprint(b));

    std::vector<Entity> order_a, order_b;
    a.each<Pos>([&](Entity e, Pos&) { order_a.push_back(e); });
    b.each<Pos>([&](Entity e, Pos&) { order_b.push_back(e); });
    CHECK(order_a == order_b);
}

void test_json_round_trip() {
    World a = fresh_world();
    const Entity e = a.create();
    a.add(e, Pos{1.5f, -2.25f});
    a.add(e, Tag{"say \"hi\"\n", e});

    const std::string text = to_json(a);
    CHECK(text.find("\"Pos\"") != std::string::npos);
    CHECK(text.find("\"components\"") != std::string::npos);
    CHECK(text.find("say \\\"hi\\\"\\n") != std::string::npos);

    World b = fresh_world();
    std::string error;
    const bool ok = from_json(text, b, &error);
    if (!ok) std::printf("json error: %s\n", error.c_str());
    CHECK(ok);
    CHECK(b.alive(e));
    CHECK(b.get<Pos>(e) && b.get<Pos>(e)->x == 1.5f && b.get<Pos>(e)->y == -2.25f);
    CHECK(b.get<Tag>(e) && b.get<Tag>(e)->name == "say \"hi\"\n");
    CHECK(fingerprint(a) == fingerprint(b));
}

struct Settings {
    i32 volume = 50;
    bool fullscreen = false;
    std::string name = "default";
    u64 big = 0;
    double ratio = 1.0;
    std::vector<i32> keys;
    Pos spawn;
    float debug = 7;

    void reflect(Visitor& v) {
        v.field("volume", volume);
        v.field("fullscreen", fullscreen);
        v.field("name", name);
        v.field("big", big);
        v.field("ratio", ratio);
        v.field("keys", keys);
        v.field("spawn", spawn);
        v.field("debug", debug, {.transient = true});
    }
};

void test_json_tolerance() {
    // Missing fields keep defaults, unknown fields are ignored, order is free,
    // comments and trailing commas are tolerated.
    const char* text = R"({
        // a comment
        "name": "custom",
        "unknown": [1, 2, 3],
        "keys": [4, 5, 6,],
        "spawn": {"y": 9},
        "big": 18446744073709551615,
        "ratio": 0.1,
    })";
    Settings s;
    std::string error;
    const bool ok = from_json(text, s, &error);
    if (!ok) std::printf("json error: %s\n", error.c_str());
    CHECK(ok);
    CHECK(s.volume == 50);
    CHECK(s.name == "custom");
    CHECK(s.keys == std::vector<i32>({4, 5, 6}));
    CHECK(s.spawn.x == 0 && s.spawn.y == 9);
    CHECK(s.big == 18446744073709551615ull);
    CHECK(s.ratio == 0.1);

    // Transient fields are not written.
    const std::string out = to_json(s);
    CHECK(out.find("debug") == std::string::npos);
    CHECK(out.find("\"volume\": 50") != std::string::npos);

    // Parse errors are reported with a position.
    Settings t;
    CHECK(!from_json("{ \"volume\": }", t, &error));
    CHECK(error.find("line 1") != std::string::npos);
    CHECK(!from_json("{} trailing", t, &error));
}

void test_json_file() {
    Settings s;
    s.volume = 11;
    s.keys = {1, 2};
    const std::string path = "ecs_test_settings.json";
    std::string error;
    CHECK(save_json_file(path, s, &error));
    Settings loaded;
    CHECK(load_json_file(path, loaded, &error));
    CHECK(loaded.volume == 11 && loaded.keys == s.keys);
    std::remove(path.c_str());
    CHECK(!load_json_file("does_not_exist.json", loaded, &error));
    CHECK(!error.empty());
}

} // namespace

int main() {
    test_entity_lifetime();
    test_components();
    test_each();
    test_reflect_round_trip();
    test_determinism();
    test_json_round_trip();
    test_json_tolerance();
    test_json_file();

    if (failures == 0) {
        std::printf("ecs_test: all checks passed\n");
        return 0;
    }
    std::printf("ecs_test: %d check(s) failed\n", failures);
    return 1;
}
