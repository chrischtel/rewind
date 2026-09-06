#pragma once

// An optional parent link between entities. Register the component to opt
// in (`world.register_component<Parent>("Parent")`), then the editor shows
// the Entities panel as a tree and lets you reparent by dragging. The link
// is organisational: nothing moves children when a parent moves unless
// your tick does it, so transforms stay exactly what your game says.

#include "rewind/sim/reflect.h"
#include "rewind/sim/world.h"

#include <vector>

namespace rw {

struct Parent {
    Entity entity; ///< null = root

    void reflect(Visitor& v) { v.field("entity", entity); }
};

/// The parent, or a null entity.
inline Entity parent_of(World& world, Entity e) {
    const Parent* p = world.get<Parent>(e);
    return p ? p->entity : Entity::null();
}

/// Whether `ancestor` is above `e` (any number of levels). Stops on cycles.
inline bool is_ancestor(World& world, Entity ancestor, Entity e) {
    Entity cur = parent_of(world, e);
    for (usize guard = 0; !cur.is_null() && guard < 4096; ++guard) {
        if (cur == ancestor) return true;
        cur = parent_of(world, cur);
    }
    return false;
}

/// Set or clear (null) the parent. Refuses self and cycles, returns false then.
inline bool set_parent(World& world, Entity child, Entity parent) {
    if (!world.alive(child)) return false;
    if (parent.is_null()) {
        if (world.has<Parent>(child)) world.remove<Parent>(child);
        return true;
    }
    if (!world.alive(parent) || parent == child || is_ancestor(world, child, parent)) return false;
    world.add<Parent>(child, Parent{parent});
    return true;
}

/// Direct children, in entity order.
inline std::vector<Entity> children_of(World& world, Entity parent) {
    std::vector<Entity> out;
    world.each<Parent>([&](Entity e, Parent& p) {
        if (p.entity == parent) out.push_back(e);
    });
    return out;
}

/// Every descendant, depth first.
inline void descendants_of(World& world, Entity parent, std::vector<Entity>& out) {
    for (Entity c : children_of(world, parent)) {
        out.push_back(c);
        descendants_of(world, c, out);
    }
}

/// Links whose parent no longer exists become roots. Call after destroying entities.
inline usize detach_orphans(World& world) {
    std::vector<Entity> orphans;
    world.each<Parent>([&](Entity e, Parent& p) {
        if (!world.alive(p.entity)) orphans.push_back(e);
    });
    for (Entity e : orphans) world.remove<Parent>(e);
    return orphans.size();
}

/// Destroy an entity and everything below it.
inline void destroy_with_children(World& world, Entity e) {
    std::vector<Entity> below;
    descendants_of(world, e, below);
    for (Entity c : below) world.destroy(c);
    world.destroy(e);
}

} // namespace rw
