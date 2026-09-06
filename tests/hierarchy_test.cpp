// Headless checks for the optional entity hierarchy and its survival through scenes.

#include <rewind/project/scene.h>
#include <rewind/sim/hierarchy.h>
#include <rewind/sim/world.h>

#include <algorithm>
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

struct Pos {
    float x = 0, y = 0;
    void reflect(Visitor& v) {
        v.field("x", x);
        v.field("y", y);
    }
};

void test_links() {
    World w;
    w.register_component<Pos>("Pos");
    w.register_component<Parent>("Parent");
    const Entity root = w.create(), a = w.create(), b = w.create(), c = w.create();
    CHECK(parent_of(w, a).is_null());
    CHECK(set_parent(w, a, root) && set_parent(w, b, root) && set_parent(w, c, a));
    CHECK(parent_of(w, c) == a && parent_of(w, a) == root);
    CHECK(children_of(w, root).size() == 2);
    std::vector<Entity> all;
    descendants_of(w, root, all);
    CHECK(all.size() == 3);
    CHECK(is_ancestor(w, root, c) && !is_ancestor(w, c, root));

    // Cycles and self-parenting are refused.
    CHECK(!set_parent(w, root, c));
    CHECK(!set_parent(w, a, a));
    CHECK(parent_of(w, root).is_null());

    // Detach, orphans, destroy with children.
    CHECK(set_parent(w, b, Entity::null()) && !w.has<Parent>(b));
    w.destroy(a);
    CHECK(detach_orphans(w) == 1 && parent_of(w, c).is_null());
    const Entity d = w.create();
    CHECK(set_parent(w, d, root));
    destroy_with_children(w, root);
    CHECK(!w.alive(root) && !w.alive(d) && w.alive(c));
}

void test_through_scene() {
    World w;
    w.register_component<Pos>("Pos");
    w.register_component<Parent>("Parent");
    const Entity root = w.create();
    w.add<Pos>(root, Pos{1, 2});
    const Entity child = w.create();
    w.add<Pos>(child, Pos{3, 4});
    set_parent(w, child, root);

    const Scene scene = capture_scene(w, "test", nullptr);
    World loaded;
    loaded.register_component<Pos>("Pos");
    loaded.register_component<Parent>("Parent");
    SceneWarnings warnings;
    apply_scene(scene, loaded, nullptr, &warnings);
    CHECK(warnings.empty());
    CHECK(parent_of(loaded, child) == root);
    CHECK(children_of(loaded, root).size() == 1 && children_of(loaded, root)[0] == child);
}

} // namespace

int main() {
    test_links();
    test_through_scene();
    if (failures == 0) std::printf("hierarchy_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
