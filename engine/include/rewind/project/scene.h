#pragma once

#include "rewind/core/types.h"
#include "rewind/sim/reflect.h"
#include "rewind/sim/text_format.h"
#include "rewind/sim/world.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace rw {

/// Marks an entity as an instance of a prefab. Register it in your world if
/// you use prefabs; scenes then store only what the instance changed.
struct PrefabLink {
    std::string prefab; ///< prefab id

    void reflect(Visitor& v) { v.field("prefab", prefab, {.read_only = true}); }
};

/// One entity template: components by pool name, as JSON, plus an id that
/// scenes refer to. Stored as scenes/prefabs/<name>.json.
struct Prefab {
    std::string id;
    std::string name;
    JsonValue components; ///< object: { "Transform": {...}, "Shape": {...} }
};

/// One entity in a scene file. Components hold either the full values or,
/// for prefab instances, only the fields that differ from the prefab.
struct SceneEntity {
    Entity handle;
    std::string prefab;               ///< empty unless the entity is a prefab instance
    JsonValue components;             ///< object keyed by pool name
    std::vector<std::string> removed; ///< prefab components this instance dropped
};

/// A level: entities by name-keyed components, plus a free-form settings
/// block the game reads with apply_json into its own reflected struct.
struct Scene {
    std::string name;
    std::vector<SceneEntity> entities;
    JsonValue settings;
};

/// What apply_scene could not honour; loading continues past these.
struct SceneWarnings {
    std::vector<std::string> items;
    bool empty() const { return items.empty(); }
};

class PrefabLibrary {
public:
    void add(Prefab prefab) { prefabs_[prefab.id] = std::move(prefab); }
    const Prefab* find(std::string_view id) const;
    usize size() const { return prefabs_.size(); }
    const std::map<std::string, Prefab>& all() const { return prefabs_; }

    /// Read every *.json in `dir` as a prefab. Missing dir is fine.
    void load_dir(const std::string& dir);

private:
    std::map<std::string, Prefab> prefabs_;
};

// ---- components as JSON ---------------------------------------------------------------

/// All components of `e` as an object keyed by pool name.
JsonValue entity_to_json(World& world, Entity e);

/// Apply an object keyed by pool name onto `e`, adding components that are
/// missing. Unknown pool names go to `warnings`.
void entity_from_json(World& world, Entity e, const JsonValue& components, SceneWarnings* warnings = nullptr);

// ---- prefabs ----------------------------------------------------------------------------

/// Snapshot an entity's components into a prefab (the PrefabLink itself is left out).
Prefab make_prefab(World& world, Entity e, std::string_view id, std::string_view name);

/// Create an entity from a prefab and link it.
Entity spawn_prefab(World& world, const Prefab& prefab, SceneWarnings* warnings = nullptr);

std::string prefab_to_json(const Prefab& prefab);
bool prefab_from_json(std::string_view text, Prefab& out, std::string* error = nullptr);
bool save_prefab(const std::string& path, const Prefab& prefab, std::string* error = nullptr);
bool load_prefab(const std::string& path, Prefab& out, std::string* error = nullptr);

// ---- scenes -----------------------------------------------------------------------------

/// Snapshot a world into a scene. With a prefab library, instances (entities
/// with a PrefabLink to a known prefab) store only their differences.
Scene capture_scene(World& world, std::string_view name, const PrefabLibrary* prefabs = nullptr);

/// Replace the world's contents with the scene. Entity handles are restored
/// exactly, so references between entities stay valid. Missing prefabs and
/// unknown component names are reported in `warnings` and skipped.
void apply_scene(const Scene& scene, World& world, const PrefabLibrary* prefabs = nullptr,
                 SceneWarnings* warnings = nullptr);

std::string scene_to_json(const Scene& scene);
bool scene_from_json(std::string_view text, Scene& out, std::string* error = nullptr);
bool save_scene(const std::string& path, const Scene& scene, std::string* error = nullptr);
bool load_scene(const std::string& path, Scene& out, std::string* error = nullptr);

/// Convenience for games: read a reflected settings struct out of the scene.
template <class T> void scene_settings(const Scene& scene, T& out) { apply_json(scene.settings, out); }
template <Reflectable T> void set_scene_settings(Scene& scene, T& settings) { scene.settings = to_json_value(settings); }

} // namespace rw
