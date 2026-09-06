// Headless checks for scenes and prefabs: entity JSON, prefab spawning,
// overrides, handle preservation, warnings, and file round trips.

#include <rewind/project/scene.h>
#include <rewind/sim/reflect.h>
#include <rewind/sim/serialize.h>
#include <rewind/sim/world.h>

#include <cstdio>
#include <filesystem>
#include <string>

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

struct Health {
    i32 hp = 100;
    i32 max = 100;
    void reflect(Visitor& v) {
        v.field("hp", hp);
        v.field("max", max);
    }
};

struct Follow {
    Entity target;
    float speed = 1;
    void reflect(Visitor& v) {
        v.field("target", target);
        v.field("speed", speed);
    }
};

struct LevelSettings {
    std::string music = "none";
    float gravity = 9.8f;
    void reflect(Visitor& v) {
        v.field("music", music);
        v.field("gravity", gravity);
    }
};

World fresh() {
    World w;
    w.register_component<Pos>("Pos");
    w.register_component<Health>("Health");
    w.register_component<Follow>("Follow");
    w.register_component<PrefabLink>("PrefabLink");
    return w;
}

u64 fingerprint(World& w) {
    Writer out;
    save_reflected(w, out);
    return hash_bytes(out.bytes());
}

void test_components_by_name() {
    World w = fresh();
    const Entity e = w.create();
    CHECK(!w.has_component(e, "Pos"));
    CHECK(w.add_component(e, "Pos"));
    CHECK(w.has_component(e, "Pos"));
    CHECK(!w.add_component(e, "Nope"));
    w.get<Pos>(e)->x = 5;

    const JsonValue j = entity_to_json(w, e);
    CHECK(j.type == JsonValue::Type::Object && j.members.size() == 1);
    CHECK(j.find("Pos") && j.find("Pos")->find("x") && j.find("Pos")->find("x")->text == "5");

    const auto names = w.component_names(e);
    CHECK(names.size() == 1 && names[0] == "Pos");
    CHECK(w.remove_component(e, "Pos") && !w.has_component(e, "Pos"));

    // Applying JSON adds missing components and warns about unknown ones.
    JsonValue comps;
    std::string error;
    parse_json(R"({"Health": {"hp": 7}, "Ghost": {"a": 1}})", comps, error);
    SceneWarnings warnings;
    entity_from_json(w, e, comps, &warnings);
    CHECK(w.has_component(e, "Health") && w.get<Health>(e)->hp == 7 && w.get<Health>(e)->max == 100);
    CHECK(warnings.items.size() == 1 && warnings.items[0].find("Ghost") != std::string::npos);
}

void test_scene_round_trip_keeps_handles() {
    World a = fresh();
    const Entity e0 = a.create();
    const Entity e1 = a.create();
    const Entity e2 = a.create();
    a.add(e0, Pos{1, 2});
    a.add(e1, Pos{3, 4});
    a.add(e1, Health{50, 80});
    a.add(e2, Follow{e1, 2.5f});
    a.destroy(e0);           // leaves a hole
    const Entity e3 = a.create(); // reuses slot 0 with generation 2
    a.add(e3, Pos{9, 9});

    const Scene scene = capture_scene(a, "test");
    CHECK(scene.name == "test" && scene.entities.size() == 3);
    const std::string text = scene_to_json(scene);
    CHECK(text.find("\"Follow\"") != std::string::npos);

    Scene loaded;
    std::string error;
    CHECK(scene_from_json(text, loaded, &error));
    World b = fresh();
    SceneWarnings warnings;
    apply_scene(loaded, b, nullptr, &warnings);
    CHECK(warnings.empty());
    CHECK(b.entity_count() == 3);
    CHECK(b.alive(e1) && b.alive(e2) && b.alive(e3) && !b.alive(e0));
    CHECK(b.get<Pos>(e1) && b.get<Pos>(e1)->y == 4);
    CHECK(b.get<Health>(e1) && b.get<Health>(e1)->max == 80);
    CHECK(b.get<Follow>(e2) && b.get<Follow>(e2)->target == e1 && b.alive(b.get<Follow>(e2)->target));
    CHECK(b.get<Pos>(e3) && b.get<Pos>(e3)->x == 9);

    // A scene is entity-centric: component pool order may differ from the
    // original world, but every entity is identical, and two loads of the
    // same scene are byte-identical, which is what determinism needs.
    for (const SceneEntity& se : loaded.entities)
        CHECK(write_json(entity_to_json(a, se.handle)) == write_json(entity_to_json(b, se.handle)));
    World c = fresh();
    apply_scene(loaded, c, nullptr, nullptr);
    CHECK(fingerprint(b) == fingerprint(c));

    // Slot reuse continues identically after a load.
    CHECK(a.create() == b.create());
}

void test_prefabs_and_overrides() {
    World w = fresh();
    const Entity proto = w.create();
    w.add(proto, Pos{0, 0});
    w.add(proto, Health{30, 30});
    const Prefab prefab = make_prefab(w, proto, "enemy01", "Enemy");
    CHECK(prefab.components.find("Health") && !prefab.components.find("PrefabLink"));
    w.destroy(proto);

    PrefabLibrary lib;
    lib.add(prefab);

    SceneWarnings warnings;
    const Entity a = spawn_prefab(w, *lib.find("enemy01"), &warnings);
    const Entity b = spawn_prefab(w, *lib.find("enemy01"), &warnings);
    CHECK(warnings.empty());
    CHECK(w.has_component(a, "PrefabLink") && w.get<PrefabLink>(a)->prefab == "enemy01");
    CHECK(w.get<Health>(b)->hp == 30);

    // b is customised: moved, damaged, and loses its Health entirely.
    w.get<Pos>(b)->x = 42;
    w.get<Health>(a)->hp = 12;
    w.remove_component(b, "Health");

    const Scene scene = capture_scene(w, "level", &lib);
    CHECK(scene.entities.size() == 2);
    const SceneEntity* sa = nullptr;
    const SceneEntity* sb = nullptr;
    for (const SceneEntity& se : scene.entities) {
        if (se.handle == a) sa = &se;
        if (se.handle == b) sb = &se;
    }
    CHECK(sa && sb);
    CHECK(sa->prefab == "enemy01");
    CHECK(sa->components.find("Health") && sa->components.find("Health")->find("hp"));
    CHECK(!sa->components.find("Health")->find("max")); // unchanged: not stored
    CHECK(!sa->components.find("Pos"));                 // unchanged component: not stored
    CHECK(sb->components.find("Pos") && sb->components.find("Pos")->find("x"));
    CHECK(sb->removed.size() == 1 && sb->removed[0] == "Health");

    const std::string text = scene_to_json(scene);
    Scene loaded;
    CHECK(scene_from_json(text, loaded));

    World w2 = fresh();
    apply_scene(loaded, w2, &lib, &warnings);
    CHECK(warnings.empty());
    CHECK(w2.get<Health>(a) && w2.get<Health>(a)->hp == 12 && w2.get<Health>(a)->max == 30);
    CHECK(w2.get<Pos>(b) && w2.get<Pos>(b)->x == 42);
    CHECK(!w2.has_component(b, "Health"));
    CHECK(w2.get<PrefabLink>(b) && w2.get<PrefabLink>(b)->prefab == "enemy01");
    CHECK(fingerprint(w) == fingerprint(w2));

    // Without the library the instance still loads from its overrides, with a warning.
    World w3 = fresh();
    SceneWarnings missing;
    apply_scene(loaded, w3, nullptr, &missing);
    CHECK(missing.items.size() == 2);
    CHECK(w3.get<Pos>(b) && w3.get<Pos>(b)->x == 42);
    CHECK(!w3.has_component(a, "Pos")); // came from the prefab, which is unavailable

    // Changing the prefab changes every instance that did not override the field.
    Prefab edited = prefab;
    JsonValue h;
    std::string error;
    parse_json(R"({"hp": 99, "max": 99})", h, error);
    for (auto& [k, v] : edited.components.members)
        if (k == "Health") v = h;
    PrefabLibrary lib2;
    lib2.add(edited);
    World w4 = fresh();
    apply_scene(loaded, w4, &lib2, nullptr);
    CHECK(w4.get<Health>(a)->hp == 12 && w4.get<Health>(a)->max == 99);
}

void test_settings_and_files() {
    World w = fresh();
    w.add(w.create(), Pos{1, 1});
    Scene scene = capture_scene(w, "with settings");
    LevelSettings s;
    s.music = "calm";
    s.gravity = 20;
    set_scene_settings(scene, s);

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "rewind_scene_test";
    std::filesystem::create_directories(dir);
    const std::string scene_path = (dir / "level.json").string();
    std::string error;
    CHECK(save_scene(scene_path, scene, &error));
    Scene back;
    CHECK(load_scene(scene_path, back, &error));
    LevelSettings loaded;
    scene_settings(back, loaded);
    CHECK(loaded.music == "calm" && loaded.gravity == 20);
    CHECK(back.name == "with settings" && back.entities.size() == 1);

    // Prefab files and the library directory loader.
    const Entity e = w.create();
    w.add(e, Health{5, 5});
    const Prefab p = make_prefab(w, e, "hp5", "Weakling");
    std::filesystem::create_directories(dir / "prefabs");
    CHECK(save_prefab((dir / "prefabs" / "weakling.json").string(), p, &error));
    PrefabLibrary lib;
    lib.load_dir((dir / "prefabs").string());
    CHECK(lib.size() == 1 && lib.find("hp5") && lib.find("hp5")->name == "Weakling");
    Prefab bad;
    CHECK(!prefab_from_json("{\"name\": \"no id\"}", bad, &error));
    CHECK(!load_scene((dir / "missing.json").string(), back, &error));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

} // namespace

int main() {
    test_components_by_name();
    test_scene_round_trip_keeps_handles();
    test_prefabs_and_overrides();
    test_settings_and_files();

    if (failures == 0) {
        std::printf("scene_test: all checks passed\n");
        return 0;
    }
    std::printf("scene_test: %d check(s) failed\n", failures);
    return 1;
}
