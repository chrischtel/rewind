#include "rewind/project/scene.h"

#include "rewind/core/log.h"

#include <filesystem>

namespace rw {

namespace {

constexpr const char* kLinkPool = "PrefabLink";

JsonValue object() {
    JsonValue v;
    v.type = JsonValue::Type::Object;
    return v;
}

JsonValue array() {
    JsonValue v;
    v.type = JsonValue::Type::Array;
    return v;
}

JsonValue string_value(std::string_view s) {
    JsonValue v;
    v.type = JsonValue::Type::String;
    v.text = std::string(s);
    return v;
}

JsonValue number_value(u64 n) {
    JsonValue v;
    v.type = JsonValue::Type::Number;
    v.text = std::to_string(n);
    return v;
}

void set_member(JsonValue& obj, const std::string& key, JsonValue value) {
    for (auto& [k, v] : obj.members)
        if (k == key) {
            v = std::move(value);
            return;
        }
    obj.members.emplace_back(key, std::move(value));
}

u64 number_of(const JsonValue* v) { return v && v->type == JsonValue::Type::Number ? std::strtoull(v->text.c_str(), nullptr, 10) : 0; }
std::string string_of(const JsonValue* v) { return v && v->type == JsonValue::Type::String ? v->text : std::string(); }

/// One component of one entity as JSON.
JsonValue component_json(World& world, Entity e, const std::string& name) {
    JsonWriteVisitor w;
    world.reflect_component(e, name, w);
    JsonValue v;
    std::string error;
    parse_json(w.finish(), v, error);
    return v;
}

void warn(SceneWarnings* warnings, std::string text) {
    RW_LOG_WARN("scene", "%s", text.c_str());
    if (warnings) warnings->items.push_back(std::move(text));
}

} // namespace

// ---- prefab library --------------------------------------------------------------------

const Prefab* PrefabLibrary::find(std::string_view id) const {
    auto it = prefabs_.find(std::string(id));
    return it == prefabs_.end() ? nullptr : &it->second;
}

void PrefabLibrary::load_dir(const std::string& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
        Prefab p;
        std::string error;
        if (load_prefab(entry.path().string(), p, &error)) add(std::move(p));
        else RW_LOG_WARN("scene", "prefab %s: %s", entry.path().filename().string().c_str(), error.c_str());
    }
}

// ---- entity <-> json -------------------------------------------------------------------

JsonValue entity_to_json(World& world, Entity e) {
    JsonValue out = object();
    for (const std::string& name : world.component_names(e)) set_member(out, name, component_json(world, e, name));
    return out;
}

void entity_from_json(World& world, Entity e, const JsonValue& components, SceneWarnings* warnings) {
    if (components.type != JsonValue::Type::Object) return;
    for (const auto& [name, value] : components.members) {
        if (!world.has_component(e, name) && !world.add_component(e, name)) {
            warn(warnings, "unknown component '" + name + "' skipped");
            continue;
        }
        JsonLoadVisitor v(value);
        world.reflect_component(e, name, v);
    }
}

// ---- prefabs -----------------------------------------------------------------------------

Prefab make_prefab(World& world, Entity e, std::string_view id, std::string_view name) {
    Prefab p;
    p.id = std::string(id);
    p.name = std::string(name);
    p.components = entity_to_json(world, e);
    // The link belongs to instances, not to the template.
    auto& m = p.components.members;
    for (usize i = 0; i < m.size(); ++i)
        if (m[i].first == kLinkPool) {
            m.erase(m.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    return p;
}

Entity spawn_prefab(World& world, const Prefab& prefab, SceneWarnings* warnings) {
    const Entity e = world.create();
    entity_from_json(world, e, prefab.components, warnings);
    JsonValue link = object();
    set_member(link, "prefab", string_value(prefab.id));
    JsonValue wrapper = object();
    set_member(wrapper, kLinkPool, std::move(link));
    if (world.has_pool(kLinkPool)) entity_from_json(world, e, wrapper, warnings);
    return e;
}

std::string prefab_to_json(const Prefab& prefab) {
    JsonValue doc = object();
    set_member(doc, "id", string_value(prefab.id));
    set_member(doc, "name", string_value(prefab.name));
    set_member(doc, "components", prefab.components);
    return write_json(doc);
}

bool prefab_from_json(std::string_view text, Prefab& out, std::string* error) {
    JsonValue doc;
    std::string err;
    if (!parse_json(text, doc, err)) {
        if (error) *error = err;
        return false;
    }
    out = Prefab{};
    out.id = string_of(doc.find("id"));
    out.name = string_of(doc.find("name"));
    if (const JsonValue* c = doc.find("components")) out.components = *c;
    else out.components = object();
    if (out.id.empty()) {
        if (error) *error = "prefab has no id";
        return false;
    }
    return true;
}

bool save_prefab(const std::string& path, const Prefab& prefab, std::string* error) {
    return write_text_file(path, prefab_to_json(prefab), error);
}

bool load_prefab(const std::string& path, Prefab& out, std::string* error) {
    std::string text;
    if (!read_text_file(path, text, error)) return false;
    return prefab_from_json(text, out, error);
}

// ---- scenes ------------------------------------------------------------------------------

Scene capture_scene(World& world, std::string_view name, const PrefabLibrary* prefabs) {
    Scene scene;
    scene.name = std::string(name);
    scene.settings = object();
    world.each_entity([&](Entity e) {
        SceneEntity se;
        se.handle = e;
        JsonValue full = entity_to_json(world, e);

        const Prefab* prefab = nullptr;
        if (const JsonValue* link = full.find(kLinkPool)) {
            se.prefab = string_of(link->find("prefab"));
            if (prefabs) prefab = prefabs->find(se.prefab);
        }
        if (prefab) {
            // Store only what differs from the template, and note dropped components.
            JsonValue diff = object();
            for (const auto& [comp, value] : full.members) {
                if (comp == kLinkPool) continue;
                const JsonValue* base = prefab->components.find(comp);
                if (!base) {
                    set_member(diff, comp, value);
                    continue;
                }
                JsonValue d = json_diff(*base, value);
                if (!d.members.empty()) set_member(diff, comp, std::move(d));
            }
            for (const auto& [comp, value] : prefab->components.members)
                if (!full.find(comp)) se.removed.push_back(comp);
            se.components = std::move(diff);
        } else {
            se.components = std::move(full);
        }
        scene.entities.push_back(std::move(se));
    });
    return scene;
}

void apply_scene(const Scene& scene, World& world, const PrefabLibrary* prefabs, SceneWarnings* warnings) {
    world.clear();
    for (const SceneEntity& se : scene.entities) {
        if (!world.create_with(se.handle)) {
            warn(warnings, "entity " + std::to_string(se.handle.index) + " could not be restored");
            continue;
        }
        const Prefab* prefab = nullptr;
        if (!se.prefab.empty()) {
            prefab = prefabs ? prefabs->find(se.prefab) : nullptr;
            if (!prefab) warn(warnings, "prefab '" + se.prefab + "' not found; instance loaded with its overrides only");
        }
        if (prefab) {
            entity_from_json(world, se.handle, prefab->components, warnings);
            for (const std::string& comp : se.removed) world.remove_component(se.handle, comp);
        }
        entity_from_json(world, se.handle, se.components, warnings);
        if (!se.prefab.empty() && world.has_pool(kLinkPool)) {
            JsonValue link = object();
            set_member(link, "prefab", string_value(se.prefab));
            JsonValue wrapper = object();
            set_member(wrapper, kLinkPool, std::move(link));
            entity_from_json(world, se.handle, wrapper, warnings);
        }
    }
    world.rebuild_free_list();
}

std::string scene_to_json(const Scene& scene) {
    JsonValue doc = object();
    set_member(doc, "name", string_value(scene.name));
    JsonValue entities = array();
    for (const SceneEntity& se : scene.entities) {
        JsonValue e = object();
        set_member(e, "index", number_value(se.handle.index));
        set_member(e, "generation", number_value(se.handle.generation));
        if (!se.prefab.empty()) set_member(e, "prefab", string_value(se.prefab));
        if (!se.removed.empty()) {
            JsonValue removed = array();
            for (const std::string& r : se.removed) removed.items.push_back(string_value(r));
            set_member(e, "removed", std::move(removed));
        }
        set_member(e, "components", se.components);
        entities.items.push_back(std::move(e));
    }
    set_member(doc, "entities", std::move(entities));
    set_member(doc, "settings", scene.settings.type == JsonValue::Type::Null ? object() : scene.settings);
    return write_json(doc);
}

bool scene_from_json(std::string_view text, Scene& out, std::string* error) {
    JsonValue doc;
    std::string err;
    if (!parse_json(text, doc, err)) {
        if (error) *error = err;
        return false;
    }
    out = Scene{};
    out.name = string_of(doc.find("name"));
    if (const JsonValue* entities = doc.find("entities")) {
        for (const JsonValue& e : entities->items) {
            SceneEntity se;
            se.handle.index = static_cast<u32>(number_of(e.find("index")));
            se.handle.generation = static_cast<u32>(number_of(e.find("generation")));
            if (se.handle.generation == 0) se.handle.generation = 1;
            se.prefab = string_of(e.find("prefab"));
            if (const JsonValue* removed = e.find("removed"))
                for (const JsonValue& r : removed->items) se.removed.push_back(r.text);
            if (const JsonValue* c = e.find("components")) se.components = *c;
            else se.components = object();
            out.entities.push_back(std::move(se));
        }
    }
    if (const JsonValue* s = doc.find("settings")) out.settings = *s;
    else out.settings = object();
    return true;
}

bool save_scene(const std::string& path, const Scene& scene, std::string* error) {
    return write_text_file(path, scene_to_json(scene), error);
}

bool load_scene(const std::string& path, Scene& out, std::string* error) {
    std::string text;
    if (!read_text_file(path, text, error)) return false;
    return scene_from_json(text, out, error);
}

} // namespace rw
