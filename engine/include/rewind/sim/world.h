#pragma once

#include "rewind/core/types.h"
#include "rewind/sim/reflect.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rw {

/// Handle to an entity. Stale handles (destroyed, slot reused) are detected
/// through the generation, so a Game can keep entity references in its
/// state without dangling.
struct Entity {
    u32 index = 0;
    u32 generation = 0; ///< 0 = null handle; live generations start at 1.

    static constexpr Entity null() { return {}; }
    bool is_null() const { return generation == 0; }
    bool operator==(const Entity&) const = default;

    void reflect(Visitor& v) {
        v.field("index", index);
        v.field("generation", generation);
    }
};

/// Entities plus dense, typed component pools. Plain simulation state: keep
/// it inside your Game and list it in reflect(); it saves, loads, rewinds,
/// hot-reloads and shows up in the inspector like any other field.
///
/// Components are ordinary structs with a reflect() function. Register them
/// once (constructor of your Game is a good place) so pools always come in
/// the same order; unregistered types are registered on first use.
///
/// Iteration order is the order of the first pool named in each(), which
/// only changes through create/add/remove/destroy, so it is deterministic.
/// Do not add or remove components of the iterated types inside each().
class World {
public:
    template <class T> void register_component(std::string_view name) {
        const std::type_index key(typeid(T));
        if (type_to_pool_.contains(key)) return;
        auto pool = std::make_unique<Pool<T>>();
        pool->name.assign(name.data(), name.size());
        type_to_pool_[key] = pools_.size();
        pools_.push_back(std::move(pool));
    }

    // ---- entities -------------------------------------------------------------
    Entity create() {
        u32 index;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            index = static_cast<u32>(slots_.size());
            slots_.push_back(Slot{});
        }
        Slot& s = slots_[index];
        ++s.generation;
        if (s.generation == 0) s.generation = 1;
        s.alive = true;
        ++alive_count_;
        return Entity{index, s.generation};
    }

    void destroy(Entity e) {
        if (!alive(e)) return;
        for (auto& p : pools_) p->remove(e.index);
        slots_[e.index].alive = false;
        free_.push_back(e.index);
        --alive_count_;
    }

    bool alive(Entity e) const {
        return !e.is_null() && e.index < slots_.size() && slots_[e.index].alive &&
               slots_[e.index].generation == e.generation;
    }

    usize entity_count() const { return alive_count_; }

    /// Every live entity, in slot order.
    template <class F> void each_entity(F&& f) {
        for (u32 i = 0; i < slots_.size(); ++i)
            if (slots_[i].alive) f(Entity{i, slots_[i].generation});
    }

    // ---- components -----------------------------------------------------------
    /// Add (or replace) a component. Returns a reference valid until the next
    /// add or remove of the same component type.
    template <class T> T& add(Entity e, T value = {}) {
        if (!alive(e)) {
            std::fprintf(stderr, "World::add on a dead entity\n");
            std::abort();
        }
        return pool<T>().add(e.index, std::move(value));
    }

    template <class T> T* get(Entity e) { return alive(e) ? pool<T>().get(e.index) : nullptr; }
    template <class T> const T* get(Entity e) const {
        return alive(e) ? const_cast<World*>(this)->pool<T>().get(e.index) : nullptr;
    }
    template <class T> bool has(Entity e) const {
        return alive(e) && const_cast<World*>(this)->pool<T>().has(e.index);
    }
    template <class T> void remove(Entity e) {
        if (alive(e)) pool<T>().remove(e.index);
    }
    template <class T> usize count() { return pool<T>().data.size(); }

    /// Call f(Entity, Ts&...) for every entity that has all of Ts.
    template <class... Ts, class F> void each(F&& f) {
        static_assert(sizeof...(Ts) > 0, "each() needs at least one component type");
        using First = std::tuple_element_t<0, std::tuple<Ts...>>;
        Pool<First>& primary = pool<First>();
        for (usize i = 0; i < primary.data.size(); ++i) {
            const u32 idx = primary.owners[i];
            if (!(pool<Ts>().has(idx) && ...)) continue;
            f(Entity{idx, slots_[idx].generation}, *pool<Ts>().get(idx)...);
        }
    }

    void clear() {
        slots_.clear();
        free_.clear();
        alive_count_ = 0;
        for (auto& p : pools_) p->clear();
    }

    /// Bring an entity back with exactly this handle (scene loading). Fails
    /// if the slot is already alive.
    bool create_with(Entity handle) {
        if (handle.is_null()) return false;
        if (handle.index >= slots_.size()) slots_.resize(static_cast<usize>(handle.index) + 1);
        Slot& s = slots_[handle.index];
        if (s.alive) return false;
        s.generation = handle.generation;
        s.alive = true;
        ++alive_count_;
        rebuild_free_list();
        return true;
    }

    void rebuild_free_list() {
        free_.clear();
        for (u32 i = static_cast<u32>(slots_.size()); i-- > 0;)
            if (!slots_[i].alive) free_.push_back(i);
    }

    // ---- components by name (tools, scenes, prefabs) -----------------------------
    bool has_pool(std::string_view name) const { return pool_named(name) != nullptr; }
    bool has_component(Entity e, std::string_view name) const {
        const IPool* p = pool_named(name);
        return p && alive(e) && p->has(e.index);
    }
    /// Add a default-constructed component of the pool called `name`.
    bool add_component(Entity e, std::string_view name) {
        IPool* p = pool_named(name);
        if (!p || !alive(e)) return false;
        p->add_default(e.index);
        return true;
    }
    bool remove_component(Entity e, std::string_view name) {
        IPool* p = pool_named(name);
        if (!p || !alive(e)) return false;
        p->remove(e.index);
        return true;
    }
    /// Visit one component of one entity.
    bool reflect_component(Entity e, std::string_view name, Visitor& v) {
        IPool* p = pool_named(name);
        if (!p || !alive(e) || !p->has(e.index)) return false;
        p->reflect_component(e.index, v);
        return true;
    }
    /// Names of the components this entity has, in pool order.
    std::vector<std::string> component_names(Entity e) const {
        std::vector<std::string> names;
        if (!alive(e)) return names;
        for (const auto& p : pools_)
            if (p->has(e.index)) names.push_back(p->name);
        return names;
    }
    /// Visit every component of one entity as a struct named after its pool.
    void visit_entity(Entity e, Visitor& v) {
        if (!alive(e)) return;
        for (auto& p : pools_) {
            if (!p->has(e.index)) continue;
            if (v.begin_struct(p->name, {})) {
                p->reflect_component(e.index, v);
                v.end_struct();
            }
        }
    }

    // ---- introspection --------------------------------------------------------
    usize pool_count() const { return pools_.size(); }
    std::string_view pool_name(usize i) const { return pools_[i]->name; }
    usize pool_size(usize i) const { return pools_[i]->size(); }
    std::vector<std::string> pool_names() const {
        std::vector<std::string> names;
        for (const auto& p : pools_) names.push_back(p->name);
        return names;
    }

    void reflect(Visitor& v) {
        v.field("slots", slots_);
        v.field("free", free_);
        v.field("alive_count", alive_count_, {.read_only = true});
        for (auto& p : pools_) v.field(p->name, *p);
    }

private:
    struct Slot {
        u32 generation = 0;
        bool alive = false;
        void reflect(Visitor& v) {
            v.field("generation", generation);
            v.field("alive", alive);
        }
    };

    struct IPool {
        std::string name;
        virtual ~IPool() = default;
        virtual void remove(u32 index) = 0;
        virtual bool has(u32 index) const = 0;
        virtual usize size() const = 0;
        virtual void clear() = 0;
        virtual void reflect(Visitor& v) = 0;
        virtual void add_default(u32 index) = 0;
        virtual void reflect_component(u32 index, Visitor& v) = 0;
    };

    IPool* pool_named(std::string_view name) {
        for (auto& p : pools_)
            if (p->name == name) return p.get();
        return nullptr;
    }
    const IPool* pool_named(std::string_view name) const {
        for (const auto& p : pools_)
            if (p->name == name) return p.get();
        return nullptr;
    }

    /// Sparse set: dense component array + owner per dense slot + entity
    /// index -> dense slot lookup. Only `data` and `owners` are state; the
    /// lookup is rebuilt after every reflect so loads cannot desync it.
    template <class T> struct Pool final : IPool {
        std::vector<T> data;
        std::vector<u32> owners;
        std::vector<u32> sparse; ///< entity index -> dense index + 1, 0 = none

        bool has(u32 index) const override { return index < sparse.size() && sparse[index] != 0; }
        usize size() const override { return data.size(); }

        T* get(u32 index) { return has(index) ? &data[sparse[index] - 1] : nullptr; }

        T& add(u32 index, T value) {
            if (T* existing = get(index)) {
                *existing = std::move(value);
                return *existing;
            }
            if (sparse.size() <= index) sparse.resize(static_cast<usize>(index) + 1, 0);
            data.push_back(std::move(value));
            owners.push_back(index);
            sparse[index] = static_cast<u32>(data.size());
            return data.back();
        }

        void remove(u32 index) override {
            if (!has(index)) return;
            const usize dense = sparse[index] - 1;
            const usize last = data.size() - 1;
            if (dense != last) {
                data[dense] = std::move(data[last]);
                owners[dense] = owners[last];
                sparse[owners[dense]] = static_cast<u32>(dense + 1);
            }
            data.pop_back();
            owners.pop_back();
            sparse[index] = 0;
        }

        void clear() override {
            data.clear();
            owners.clear();
            sparse.clear();
        }

        void reflect(Visitor& v) override {
            v.field("components", data);
            v.field("owners", owners);
            rebuild_sparse();
        }

        void add_default(u32 index) override {
            if (!has(index)) add(index, T{});
        }

        void reflect_component(u32 index, Visitor& v) override {
            T* c = get(index);
            if (!c) return;
            if constexpr (Reflectable<T>) c->reflect(v); // the component's fields become the struct's fields
            else v.field("value", *c);
        }

        void rebuild_sparse() {
            sparse.clear();
            for (usize i = 0; i < owners.size(); ++i) {
                if (sparse.size() <= owners[i]) sparse.resize(static_cast<usize>(owners[i]) + 1, 0);
                sparse[owners[i]] = static_cast<u32>(i + 1);
            }
        }
    };

    template <class T> Pool<T>& pool() {
        auto it = type_to_pool_.find(std::type_index(typeid(T)));
        if (it == type_to_pool_.end()) {
            register_component<T>(typeid(T).name());
            it = type_to_pool_.find(std::type_index(typeid(T)));
        }
        return *static_cast<Pool<T>*>(pools_[it->second].get());
    }

    std::vector<Slot> slots_;
    std::vector<u32> free_;
    u32 alive_count_ = 0;
    std::vector<std::unique_ptr<IPool>> pools_;
    std::unordered_map<std::type_index, usize> type_to_pool_;
};

} // namespace rw
