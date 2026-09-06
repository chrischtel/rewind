// rewind_editor: the editor shell. Hosts a game DLL like rewind_run, but the
// game renders into a panel and the engine's tools live in dockable windows.
//
//   rewind_editor path/to/project [game arguments...]
//   rewind_editor path/to/mygame_hot.dll [game arguments...]
//
// Panels: Scene (the running game; click it to give it your input), Timeline
// (transport, scrubber, determinism), Inspector (the game's debug UI and its
// whole reflected state), Console, Profiler, Assets (the project's asset
// database and cook), and Project (settings saved to project.json).

#include <rewind/rewind.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct EditorState {
    bool has_project = false;
    rw::Project project;
    std::unique_ptr<rw::AssetDatabase> assets;
    fs::path project_file;
    fs::path state_file;
    rw::AppConfig settings;   ///< edited in the Project panel
    rw::AppConfig defaults;   ///< what the game's configure() asked for
    rw::PrefabLibrary prefabs;
    std::string scene_name = "scene";
    std::string scene_warnings;
    rw::Entity selected;
    bool show_entities = true;
    bool settings_dirty = false;
    bool layout_done = false;
    bool show_scene = true, show_timeline = true, show_inspector = true, show_console = true, show_profiler = true,
         show_project = true, show_assets = true, show_keys = false, show_settings = false, show_saves = false;
    std::string status;
    std::vector<std::string> cook_log;
    // Free camera for the scene view: pan with right/middle drag, zoom with
    // the wheel around the cursor. Overrides whatever camera the game sets.
    bool free_camera = false;
    rw::CameraController free_cam;
    rw::Camera free_cam_out;
    // Gizmos in the scene view: the selected entity's Vec2, Rect and Path
    // fields get handles; clicking near an entity's position selects it.
    bool show_gizmos = true;
    rw::Gizmos gizmos;
    bool scene_hovered = false;
    float scene_scale = 1;
    // Editing workflow: undo history over whole-state snapshots, play/stop
    // that returns to the edited state, names and search, placement.
    rw::EditHistory history;
    bool edited_frame = false;   ///< something changed the state this frame
    bool in_play = false;        ///< Play was pressed; play_start holds the state to return to
    std::vector<rw::u8> play_start;
    bool first_frame = true;
    rw::TimeMode last_mode = rw::TimeMode::Paused;
    char entity_filter[64] = "";
    rw::Vec2 context_world;      ///< world position of the last right-click
    // Scenes: the list in scenes/, the dirty flag, the unsaved-changes prompt.
    bool show_scenes = true;
    bool scene_dirty = false;
    std::vector<std::string> scene_files;
    char new_scene_name[64] = "";
    std::string pending_scene;   ///< scene to open once the unsaved prompt is answered
    bool ask_unsaved = false;
    // Welcome screen (no project or DLL given) and the recent list.
    bool welcome = false;
    std::string welcome_error;
    std::string exe_path;
    std::vector<std::string> recent;
    bool show_status_bar = true;
    bool theme_applied = false;
    std::string selected_asset; ///< path of the row picked in the Assets panel (Inspector shows its import settings)
    int tool_index = 0;         ///< 0 = Select (gizmos); n = the game's editor_tools()[n - 1]
};


EditorState g_editor;

/// The game's tool that is active, or null for the built-in selection.
rw::EditorTool* active_tool(rw::App& app) {
    if (g_editor.tool_index <= 0) return nullptr;
    const std::vector<rw::EditorTool*> tools = app.game().editor_tools();
    const rw::usize i = static_cast<rw::usize>(g_editor.tool_index - 1);
    return i < tools.size() ? tools[i] : nullptr;
}

/// The recently opened projects and DLLs, next to the executable.
struct RecentList {
    std::vector<std::string> paths;
    void reflect(rw::Visitor& v) { v.field("paths", paths); }
};

fs::path recent_file() { return fs::path(rw::executable_dir()) / "editor_recent.json"; }

void load_recent() {
    RecentList list;
    std::string error;
    if (rw::load_json_file(recent_file().string(), list, &error)) g_editor.recent = list.paths;
}

void remember_recent(const std::string& path) {
    const std::string abs = fs::absolute(path).lexically_normal().string();
    std::vector<std::string>& r = g_editor.recent;
    r.erase(std::remove(r.begin(), r.end(), abs), r.end());
    r.insert(r.begin(), abs);
    if (r.size() > 10) r.resize(10);
    RecentList list;
    list.paths = r;
    std::string error;
    rw::save_json_file(recent_file().string(), list, &error);
}

/// Start another editor on `path` and close this one.
void relaunch_with(rw::App& app, const std::string& path) {
    std::string error;
    if (rw::spawn_process({g_editor.exe_path, path}, &error)) {
        remember_recent(path);
        app.quit();
    } else {
        g_editor.welcome_error = "could not start the editor: " + error;
    }
}


std::vector<rw::u8> snapshot(rw::App& app) {
    rw::Writer w;
    app.game().save(w);
    return std::move(w.bytes());
}

void restore(rw::App& app, const std::vector<rw::u8>& bytes) {
    if (!app.load_state(bytes)) {
        g_editor.status = "could not restore that state (game code changed?)";
        return;
    }
    if (app.time_control().mode != rw::TimeMode::Playing) app.time_control().mode = rw::TimeMode::Paused;
}

/// Every edit goes through here: the timeline re-snapshots the tick, a
/// replay or rewind in progress stops, and the undo history sees the edit.
void mark_edited(rw::App& app) {
    app.timeline().recapture(app.game());
    rw::TimeControl& t = app.time_control();
    if (t.mode == rw::TimeMode::Replaying || t.mode == rw::TimeMode::Rewinding) t.mode = rw::TimeMode::Paused;
    g_editor.edited_frame = true;
    if (!g_editor.in_play) g_editor.scene_dirty = true;
}

/// First string field of an entity's components, as its display name.
class NameVisitor final : public rw::Visitor {
public:
    std::string name;
    void visit(std::string_view, bool&, const rw::FieldMeta&) override {}
    void visit(std::string_view, rw::i8&, const rw::FieldMeta&) override {}
    void visit(std::string_view, rw::i16&, const rw::FieldMeta&) override {}
    void visit(std::string_view, rw::i32&, const rw::FieldMeta&) override {}
    void visit(std::string_view, rw::i64&, const rw::FieldMeta&) override {}
    void visit(std::string_view, rw::u8&, const rw::FieldMeta&) override {}
    void visit(std::string_view, rw::u16&, const rw::FieldMeta&) override {}
    void visit(std::string_view, rw::u32&, const rw::FieldMeta&) override {}
    void visit(std::string_view, rw::u64&, const rw::FieldMeta&) override {}
    void visit(std::string_view, float&, const rw::FieldMeta&) override {}
    void visit(std::string_view, double&, const rw::FieldMeta&) override {}
    void visit(std::string_view, std::string& v, const rw::FieldMeta&) override {
        if (name.empty() && !v.empty()) name = v;
    }
    bool begin_struct(std::string_view, const rw::FieldMeta&) override { return name.empty(); }
    void end_struct() override {}
    bool begin_array(std::string_view, rw::usize&, const rw::FieldMeta&) override { return false; }
    void end_array() override {}
    bool begin_element(rw::usize) override { return false; }
    void end_element() override {}
};

std::string entity_name(rw::World& world, rw::Entity e) {
    NameVisitor v;
    world.visit_entity(e, v);
    return v.name;
}

/// Put an entity at a world position through its first Vec2-hinted field.
bool place_entity(rw::World& world, rw::Entity e, rw::Vec2 pos) {
    rw::GizmoVisitor v;
    world.visit_entity(e, v);
    for (const rw::GizmoVisitor::Target& t : v.targets()) {
        if (t.kind != rw::Widget::Vec2 || t.floats.size() < 2 || t.read_only) continue;
        *t.floats[0] = pos.x;
        *t.floats[1] = pos.y;
        return true;
    }
    return false;
}

rw::Entity duplicate_entity(rw::World& world, rw::Entity e) {
    const rw::JsonValue components = rw::entity_to_json(world, e);
    const rw::Entity copy = world.create();
    rw::entity_from_json(world, copy, components);
    return copy;
}

void delete_selected(rw::App& app) {
    rw::World* world = app.game().world();
    if (!world || !world->alive(g_editor.selected)) return;
    if (world->has_pool<rw::Parent>()) rw::destroy_with_children(*world, g_editor.selected);
    else world->destroy(g_editor.selected);
    g_editor.selected = rw::Entity::null();
    mark_edited(app);
}

void duplicate_selected(rw::App& app) {
    rw::World* world = app.game().world();
    if (!world || !world->alive(g_editor.selected)) return;
    g_editor.selected = duplicate_entity(*world, g_editor.selected);
    mark_edited(app);
}

void undo(rw::App& app) {
    if (const std::vector<rw::u8>* s = g_editor.history.undo()) restore(app, *s);
}
void redo(rw::App& app) {
    if (const std::vector<rw::u8>* s = g_editor.history.redo()) restore(app, *s);
}

// Play runs the simulation from the edited state; Stop brings that state back.
void play(rw::App& app) {
    if (!g_editor.in_play) {
        g_editor.play_start = snapshot(app);
        g_editor.in_play = true;
    }
    app.time_control().mode = rw::TimeMode::Playing;
}
void pause(rw::App& app) { app.time_control().mode = rw::TimeMode::Paused; }
void stop(rw::App& app) {
    if (g_editor.in_play) restore(app, g_editor.play_start);
    g_editor.in_play = false;
    app.time_control().mode = rw::TimeMode::Paused;
}

void default_layout(ImGuiID dockspace) {
    // Scene in the middle, inspector on the right, tools along the bottom.
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspace;
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.32f, nullptr, &center);

    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.2f, nullptr, &center);
    ImGui::DockBuilderDockWindow("Scene", center);
    ImGui::DockBuilderDockWindow("Entities", left);
    ImGui::DockBuilderDockWindow("Scenes", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Project", right);
    ImGui::DockBuilderDockWindow("Timeline", bottom);
    ImGui::DockBuilderDockWindow("Assets", bottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Profiler", bottom);
    ImGui::DockBuilderDockWindow("Keys", bottom);
    ImGui::DockBuilderFinish(dockspace);
}

void save_project(rw::App& app) {
    std::string error;
    bool ok;
    if (g_editor.has_project) {
        rw::Project p = g_editor.project;
        p.app = g_editor.settings;
        ok = rw::save_project(p, &g_editor.defaults, &error);
    } else {
        ok = rw::save_json_file(g_editor.project_file.string(), g_editor.settings, &error);
    }
    if (ok) {
        g_editor.settings_dirty = false;
        g_editor.status = "saved " + g_editor.project_file.filename().string() + " (applies on next start)";
        RW_LOG_INFO("editor", "%s", g_editor.status.c_str());
    } else {
        g_editor.status = error;
        RW_LOG_ERROR("editor", "%s", error.c_str());
    }
    (void)app;
}

void save_state(rw::App& app) {
    std::string error;
    const std::string json = rw::to_json(app.game());
    if (rw::write_text_file(g_editor.state_file.string(), json, &error)) {
        g_editor.status = "state saved to " + g_editor.state_file.filename().string();
        RW_LOG_INFO("editor", "%s", g_editor.status.c_str());
    } else {
        g_editor.status = error;
        RW_LOG_ERROR("editor", "%s", error.c_str());
    }
}

void load_state(rw::App& app) {
    std::string error;
    if (!rw::load_json_file(g_editor.state_file.string(), app.game(), &error)) {
        g_editor.status = error;
        RW_LOG_ERROR("editor", "%s", error.c_str());
        return;
    }
    app.timeline().recapture(app.game());
    app.time_control().mode = rw::TimeMode::Paused;
    g_editor.status = "state loaded from " + g_editor.state_file.filename().string() + " (paused)";
    RW_LOG_INFO("editor", "%s", g_editor.status.c_str());
}

fs::path scene_path(const std::string& name) {
    std::string file = name;
    if (file.size() < 5 || file.compare(file.size() - 5, 5, ".json") != 0) file += ".json";
    return g_editor.project.scene_dir() / file;
}

void reload_prefabs() {
    g_editor.prefabs = rw::PrefabLibrary{};
    g_editor.prefabs.load_dir((g_editor.project.scene_dir() / "prefabs").string());
}

void refresh_scene_list() {
    g_editor.scene_files.clear();
    if (!g_editor.has_project) return;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(g_editor.project.scene_dir(), ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
        g_editor.scene_files.push_back(entry.path().stem().string());
    }
    std::sort(g_editor.scene_files.begin(), g_editor.scene_files.end());
}

void save_scene(rw::App& app) {
    rw::World* world = app.game().world();
    if (!world) {
        g_editor.status = "this game has no world to save";
        return;
    }
    const rw::Scene scene = rw::capture_scene(*world, g_editor.scene_name, &g_editor.prefabs);
    std::string error;
    std::error_code ec;
    fs::create_directories(g_editor.project.scene_dir(), ec);
    if (rw::save_scene(scene_path(g_editor.scene_name).string(), scene, &error)) {
        g_editor.status = "scene saved: " + g_editor.scene_name + " (" + std::to_string(scene.entities.size()) + " entities)";
        RW_LOG_INFO("editor", "%s", g_editor.status.c_str());
        g_editor.scene_dirty = false;
        refresh_scene_list();
    } else {
        g_editor.status = error;
        RW_LOG_ERROR("editor", "%s", error.c_str());
    }
}

void load_scene_into(rw::App& app, const std::string& name) {
    rw::World* world = app.game().world();
    if (!world) {
        g_editor.status = "this game has no world to load into";
        return;
    }
    rw::Scene scene;
    std::string error;
    if (!rw::load_scene(scene_path(name).string(), scene, &error)) {
        g_editor.status = error;
        RW_LOG_ERROR("editor", "%s", error.c_str());
        return;
    }
    reload_prefabs();
    rw::SceneWarnings warnings;
    rw::apply_scene(scene, *world, &g_editor.prefabs, &warnings);
    app.timeline().recapture(app.game());
    g_editor.scene_name = name;
    g_editor.scene_dirty = false;
    g_editor.history.clear();
    g_editor.selected = rw::Entity::null();
    if (g_editor.in_play) g_editor.play_start = snapshot(app); // Stop now returns to this scene
    g_editor.scene_warnings.clear();
    for (const std::string& w : warnings.items) g_editor.scene_warnings += w + "\n";
    g_editor.status = "scene loaded: " + name + (warnings.empty() ? "" : " (with warnings, see console)");
    RW_LOG_INFO("editor", "%s", g_editor.status.c_str());
}

/// Empty world under a new name, saved right away so it shows in the list.
void new_scene(rw::App& app, const std::string& name) {
    rw::World* world = app.game().world();
    if (!world || name.empty()) return;
    world->clear();
    app.timeline().recapture(app.game());
    g_editor.scene_name = name;
    g_editor.selected = rw::Entity::null();
    g_editor.history.clear();
    save_scene(app);
}

/// Open a scene, asking first when the current one has unsaved changes.
void open_scene(rw::App& app, const std::string& name) {
    if (g_editor.scene_dirty && name != g_editor.scene_name) {
        g_editor.pending_scene = name;
        g_editor.ask_unsaved = true;
        return;
    }
    load_scene_into(app, name);
}

void draw_unsaved_prompt(rw::App& app) {
    if (g_editor.ask_unsaved) {
        ImGui::OpenPopup("Unsaved changes");
        g_editor.ask_unsaved = false;
    }
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Scene '%s' has unsaved changes.", g_editor.scene_name.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Save and open")) {
            save_scene(app);
            load_scene_into(app, g_editor.pending_scene);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            load_scene_into(app, g_editor.pending_scene);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void draw_scenes(rw::App& app) {
    if (!g_editor.show_scenes || !g_editor.has_project) return;
    if (!ImGui::Begin("Scenes", &g_editor.show_scenes)) {
        ImGui::End();
        return;
    }
    const bool has_world = app.game().world() != nullptr;
    ImGui::BeginDisabled(!has_world);
    ImGui::SetNextItemWidth(-70);
    const bool entered = ImGui::InputTextWithHint("##newname", "new scene name", g_editor.new_scene_name,
                                                  sizeof g_editor.new_scene_name, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("New") || entered) && g_editor.new_scene_name[0]) {
        if (g_editor.scene_dirty) g_editor.status = "save or discard the current scene first";
        else {
            new_scene(app, g_editor.new_scene_name);
            g_editor.new_scene_name[0] = '\0';
        }
    }
    if (ImGui::Button("Save")) save_scene(app);
    ImGui::SameLine();
    if (ImGui::Button("Save as...")) ImGui::OpenPopup("save_as");
    if (ImGui::BeginPopup("save_as")) {
        static char name[64] = "";
        if (ImGui::IsWindowAppearing()) {
            std::snprintf(name, sizeof name, "%s", g_editor.scene_name.c_str());
            ImGui::SetKeyboardFocusHere();
        }
        if (ImGui::InputText("name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue) && name[0]) {
            g_editor.scene_name = name;
            save_scene(app);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) refresh_scene_list();
    ImGui::EndDisabled();
    ImGui::Text("%s%s", g_editor.scene_name.c_str(), g_editor.scene_dirty ? " *" : "");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_editor.scene_dirty ? "(unsaved changes)" : "(saved)");
    ImGui::Separator();

    ImGui::BeginChild("##scenes");
    for (const std::string& name : g_editor.scene_files) {
        const bool current = name == g_editor.scene_name;
        std::string label = name;
        if (current && g_editor.scene_dirty) label += " *";
        if (name == g_editor.project.startup_scene) label += "  (startup)";
        if (ImGui::Selectable((label + "##s" + name).c_str(), current, ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && has_world)
            open_scene(app, name);
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Open", nullptr, false, has_world)) open_scene(app, name);
            if (ImGui::MenuItem("Use as startup scene", nullptr, name == g_editor.project.startup_scene)) {
                g_editor.project.startup_scene = name == g_editor.project.startup_scene ? "" : name;
                save_project(app);
            }
            ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("double-click to open");
    }
    if (g_editor.scene_files.empty()) ImGui::TextDisabled("no scenes yet: type a name and press New");
    ImGui::EndChild();
    ImGui::End();
}

/// The global id index the pickers and loaders use follows the database.
void refresh_asset_index() {
    if (!g_editor.assets) return;
    rw::AssetIndex& index = rw::AssetIndex::global();
    index.clear();
    for (const rw::AssetRecord& r : g_editor.assets->records()) index.add(r.meta.id, r.path, r.meta.type);
}

void cook_assets(bool force) {
    if (!g_editor.assets) return;
    g_editor.assets->scan();
    const rw::CookReport report = g_editor.assets->cook(force);
    g_editor.cook_log = report.messages;
    char buf[128];
    std::snprintf(buf, sizeof buf, "cook: %u cooked, %u up to date, %u failed", report.cooked, report.skipped,
                  report.failed);
    g_editor.status = buf;
    RW_LOG_INFO("assets", "%s", buf);
    g_editor.assets->scan();
    refresh_asset_index();
}

void draw_menu(rw::App& app) {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save project settings")) save_project(app);
        if (g_editor.has_project && ImGui::MenuItem("Cook changed assets")) cook_assets(false);
        if (g_editor.has_project && ImGui::MenuItem("Cook all assets")) cook_assets(true);
        ImGui::Separator();
        if (g_editor.has_project) {
            const bool has_world = app.game().world() != nullptr;
            if (ImGui::MenuItem(("Save scene '" + g_editor.scene_name + "'").c_str(), "Ctrl+S", false, has_world)) save_scene(app);
            if (ImGui::MenuItem("Reload scene from disk", nullptr, false, has_world)) {
                g_editor.scene_dirty = false;
                load_scene_into(app, g_editor.scene_name);
            }
            if (ImGui::MenuItem("Reload prefabs")) reload_prefabs();
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Save state as JSON")) save_state(app);
        if (ImGui::MenuItem("Load state from JSON")) load_state(app);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Esc")) app.quit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, g_editor.history.can_undo())) undo(app);
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, g_editor.history.can_redo())) redo(app);
        ImGui::Separator();
        rw::World* world = app.game().world();
        const bool has_sel = world && world->alive(g_editor.selected);
        if (ImGui::MenuItem("Duplicate entity", "Ctrl+D", false, has_sel)) duplicate_selected(app);
        if (ImGui::MenuItem("Delete entity", "Del", false, has_sel)) delete_selected(app);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Time")) {
        rw::TimeControl& t = app.time_control();
        const bool playing = t.mode == rw::TimeMode::Playing;
        if (ImGui::MenuItem(playing ? "Pause" : "Play", "F2")) t.mode = playing ? rw::TimeMode::Paused : rw::TimeMode::Playing;
        if (ImGui::MenuItem("Step one tick", "F3")) {
            t.mode = rw::TimeMode::Paused;
            t.step_request = true;
        }
        if (ImGui::MenuItem("Replay and verify", "F5")) t.replay_request = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Scene", nullptr, &g_editor.show_scene);
        ImGui::MenuItem("Entities", nullptr, &g_editor.show_entities);
        ImGui::MenuItem("Scenes", nullptr, &g_editor.show_scenes, g_editor.has_project);
        ImGui::MenuItem("Timeline", nullptr, &g_editor.show_timeline);
        ImGui::MenuItem("Inspector", nullptr, &g_editor.show_inspector);
        ImGui::MenuItem("Assets", nullptr, &g_editor.show_assets, g_editor.has_project);
        ImGui::MenuItem("Console", nullptr, &g_editor.show_console);
        ImGui::MenuItem("Profiler", nullptr, &g_editor.show_profiler);
        ImGui::MenuItem("Project", nullptr, &g_editor.show_project);
        ImGui::MenuItem("Keys", nullptr, &g_editor.show_keys);
        ImGui::MenuItem("Player settings", nullptr, &g_editor.show_settings);
        ImGui::MenuItem("Save games", nullptr, &g_editor.show_saves);
        ImGui::MenuItem("Status bar", nullptr, &g_editor.show_status_bar);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) g_editor.layout_done = false;
        ImGui::EndMenu();
    }
    const char* mode = rw::time_mode_name(app.time_control().mode);
    ImGui::SameLine(ImGui::GetWindowWidth() - 460);
    ImGui::TextDisabled("%s   tick %llu   %.0f fps   %s", mode, static_cast<unsigned long long>(app.timeline().head()),
                        app.stats().fps, g_editor.status.c_str());
    ImGui::EndMainMenuBar();
}

// Runs inside the frame right after the game drew, with its camera still
// set: gizmos for the selected entity, and click-to-select on empty space.
void draw_scene_tools(rw::App& app, rw::Renderer2D& renderer) {
    // The undo history sees every frame: the state as it is now, and
    // whether the previous frame edited it. Consecutive edits merge.
    g_editor.history.frame(g_editor.edited_frame, snapshot(app));
    g_editor.edited_frame = false;
    if (!g_editor.show_scene) return;
    rw::World* world = app.game().world();
    const ImGuiIO& io = ImGui::GetIO();
    const rw::AppConfig& cfg = app.config();
    rw::Gizmos& g = g_editor.gizmos;

    rw::GizmoInput in;
    in.mouse = renderer.window_to_render(io.MousePos.x, io.MousePos.y);
    in.down = io.MouseDown[ImGuiMouseButton_Left];
    in.alt = io.KeyAlt;
    in.shift = io.KeyShift;
    in.enabled = g_editor.scene_hovered;
    g.begin(in, renderer.camera(), static_cast<float>(cfg.render_width), static_cast<float>(cfg.render_height));

    // A game tool owns the view while it is selected.
    if (rw::EditorTool* tool = active_tool(app)) {
        rw::ToolContext ctx;
        ctx.renderer = &renderer;
        ctx.gizmos = &g;
        ctx.world = world;
        ctx.mouse_render = in.mouse;
        ctx.mouse_world = renderer.camera().to_world(in.mouse);
        ctx.hovered = g_editor.scene_hovered;
        ctx.down = in.down;
        ctx.pressed = g_editor.scene_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        ctx.released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        ctx.right_pressed = g_editor.scene_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        ctx.shift = io.KeyShift;
        ctx.ctrl = io.KeyCtrl;
        ctx.alt = io.KeyAlt;
        ctx.zoom = renderer.camera().zoom;
        ctx.dt = io.DeltaTime;
        tool->update(ctx);
        if (!ctx.status.empty()) g_editor.status = ctx.status;
        if (ctx.edited) mark_edited(app);
        g.end();
        g.draw(renderer);
        return;
    }

    if (!g_editor.show_gizmos || !world) {
        g.end();
        return;
    }

    bool edited = false;
    const rw::Entity selected = g_editor.selected;
    if (world->alive(selected)) {
        rw::u32 id_base = 1000;
        for (const std::string& name : world->component_names(selected)) {
            rw::GizmoVisitor v;
            world->reflect_component(selected, name, v);
            edited = v.apply(g, id_base) || edited;
            id_base += 1000;
        }
    }
    g.end();

    // A click on nothing selects the entity whose position is nearest (within 12 px).
    if (g.clicked_empty() && g_editor.scene_hovered) {
        const float max_dist = 12.0f / std::max(renderer.camera().zoom, 1e-6f);
        float best = max_dist;
        rw::Entity pick;
        world->each_entity([&](rw::Entity e) {
            rw::GizmoVisitor v;
            world->visit_entity(e, v);
            for (const rw::GizmoVisitor::Target& t : v.targets()) {
                if (t.kind != rw::Widget::Vec2 || t.floats.size() < 2) continue;
                const float d = rw::distance(rw::Vec2{*t.floats[0], *t.floats[1]}, g.mouse_world());
                if (d < best) {
                    best = d;
                    pick = e;
                }
            }
        });
        if (!pick.is_null()) g_editor.selected = pick;
    }

    g.draw(renderer);
    if (edited) mark_edited(app);
}

void draw_scene_context_menu(rw::App& app) {
    if (!ImGui::BeginPopup("scene_context")) return;
    rw::World* world = app.game().world();
    const rw::Vec2 at = g_editor.context_world;
    ImGui::TextDisabled("at (%.0f, %.0f)", at.x, at.y);
    ImGui::Separator();
    if (world) {
        if (ImGui::MenuItem("Create entity here")) {
            g_editor.selected = world->create();
            mark_edited(app);
        }
        if (g_editor.has_project && g_editor.prefabs.size() > 0 && ImGui::BeginMenu("Spawn prefab here")) {
            for (const auto& [id, prefab] : g_editor.prefabs.all()) {
                if (ImGui::MenuItem(prefab.name.c_str())) {
                    g_editor.selected = rw::spawn_prefab(*world, prefab);
                    place_entity(*world, g_editor.selected, at);
                    mark_edited(app);
                }
            }
            ImGui::EndMenu();
        }
        const bool has_sel = world->alive(g_editor.selected);
        ImGui::Separator();
        if (ImGui::MenuItem("Move selected here", nullptr, false, has_sel)) {
            if (place_entity(*world, g_editor.selected, at)) mark_edited(app);
            else g_editor.status = "the selected entity has no Vec2-hinted position";
        }
        if (ImGui::MenuItem("Duplicate selected here", "Ctrl+D", false, has_sel)) {
            g_editor.selected = duplicate_entity(*world, g_editor.selected);
            place_entity(*world, g_editor.selected, at);
            mark_edited(app);
        }
        if (ImGui::MenuItem("Delete selected", "Del", false, has_sel)) delete_selected(app);
        ImGui::Separator();
    }
    if (ImGui::MenuItem("Look here", nullptr, false, g_editor.free_camera)) g_editor.free_cam.set_target(at);
    ImGui::EndPopup();
}

void draw_scene(rw::App& app) {
    if (!g_editor.show_scene) {
        app.set_game_input_focus(false);
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool open = ImGui::Begin("Scene", &g_editor.show_scene);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        app.set_game_input_focus(false);
        return;
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    app.set_view_size(static_cast<int>(avail.x), static_cast<int>(avail.y));

    rw::Texture* view = app.view_texture();
    if (view && view->handle) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Image(reinterpret_cast<ImTextureID>(view->handle), ImVec2(static_cast<float>(view->width), static_cast<float>(view->height)));
        // The game target sits centred inside the view; mouse mapping needs its rect.
        const rw::AppConfig& cfg = app.config();
        const float rw_ = static_cast<float>(cfg.render_width), rh = static_cast<float>(cfg.render_height);
        float scale = std::min(static_cast<float>(view->width) / rw_, static_cast<float>(view->height) / rh);
        if (cfg.integer_scale) scale = std::max(1.0f, std::floor(scale));
        const float x = origin.x + std::floor((static_cast<float>(view->width) - rw_ * scale) * 0.5f);
        const float y = origin.y + std::floor((static_cast<float>(view->height) - rh * scale) * 0.5f);
        app.set_view_rect(rw::Rect{x, y, rw_ * scale, rh * scale});

        const bool hovered = ImGui::IsItemHovered();
        const bool focused = ImGui::IsWindowFocused();
        g_editor.scene_hovered = hovered;
        g_editor.scene_scale = scale;
        // While a gizmo is being dragged the game must not see the mouse button.
        const bool tool_active = active_tool(app) != nullptr;
        const bool gizmo_busy = tool_active || (g_editor.show_gizmos && (g_editor.gizmos.dragging() || g_editor.gizmos.hot() != 0));
        app.set_game_input_focus(focused && !gizmo_busy && (hovered || !ImGui::IsAnyItemActive()));
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ImGui::SetWindowFocus();

        // Free camera: wheel zooms around the cursor, right or middle drag pans.
        rw::Renderer2D& renderer = app.renderer();
        rw::CameraController& cam = g_editor.free_cam;
        if (g_editor.free_camera) {
            cam.set_view_size(rw_, rh);
            const ImGuiIO& io = ImGui::GetIO();
            const rw::Vec2 mouse_rt = renderer.window_to_render(io.MousePos.x, io.MousePos.y);
            if (hovered && io.MouseWheel != 0)
                cam.zoom_step(static_cast<int>(io.MouseWheel > 0 ? std::ceil(io.MouseWheel) : std::floor(io.MouseWheel)), mouse_rt);
            const bool dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0);
            if (dragging && (hovered || ImGui::IsWindowFocused()))
                cam.pan(rw::Vec2{io.MouseDelta.x / scale, io.MouseDelta.y / scale});
            cam.update(io.DeltaTime);
            g_editor.free_cam_out = cam.camera();
            renderer.set_camera_override(&g_editor.free_cam_out);
        } else if (renderer.camera_overridden()) {
            renderer.set_camera_override(nullptr);
        }

        // Right-click (without dragging) opens the placement menu; tools use right-click themselves.
        if (hovered && !tool_active && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
            if (std::fabs(drag.x) < 4 && std::fabs(drag.y) < 4) {
                const ImVec2 m = ImGui::GetIO().MousePos;
                g_editor.context_world = renderer.window_to_world(m.x, m.y);
                ImGui::OpenPopup("scene_context");
            }
        }
        draw_scene_context_menu(app);

        // Toolbar over the top-left corner of the view: transport, then camera and gizmos.
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 6, origin.y + 6));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
        {
            rw::TimeControl& t = app.time_control();
            const bool playing = t.mode == rw::TimeMode::Playing;
            const ImVec4 go(0.22f, 0.55f, 0.32f, 1.0f), go_hover(0.28f, 0.66f, 0.40f, 1.0f);
            const ImVec4 halt(0.62f, 0.26f, 0.26f, 1.0f), halt_hover(0.74f, 0.32f, 0.32f, 1.0f);
            if (!playing) {
                ImGui::PushStyleColor(ImGuiCol_Button, go);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, go_hover);
            }
            if (playing ? ImGui::Button("Pause") : ImGui::Button(g_editor.in_play ? "Resume" : "Play")) {
                if (playing) pause(app);
                else play(app);
            }
            if (!playing) ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ImGui::BeginDisabled(!g_editor.in_play);
            if (g_editor.in_play) {
                ImGui::PushStyleColor(ImGuiCol_Button, halt);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, halt_hover);
            }
            if (ImGui::Button("Stop")) stop(app);
            if (g_editor.in_play) ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Step")) {
                t.mode = rw::TimeMode::Paused;
                t.step_request = true;
            }
            ImGui::SameLine();
            if (g_editor.in_play) ImGui::TextDisabled("%s", playing ? "playing" : "paused (Stop returns to the edited state)");
            else if (g_editor.has_project) ImGui::TextDisabled("editing %s%s", g_editor.scene_name.c_str(), g_editor.scene_dirty ? " *" : "");
            else ImGui::TextDisabled("editing");
            ImGui::SameLine(0, 16);
            ImGui::BeginDisabled(!g_editor.history.can_undo());
            if (ImGui::SmallButton("Undo")) undo(app);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!g_editor.history.can_redo());
            if (ImGui::SmallButton("Redo")) redo(app);
            ImGui::EndDisabled();
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 6, origin.y + 6 + ImGui::GetFrameHeight() + 4));
        if (ImGui::Checkbox("Free camera", &g_editor.free_camera) && g_editor.free_camera) {
            // Start from where the game is looking right now.
            const rw::Camera& current = renderer.camera();
            cam.set_view_size(rw_, rh);
            cam.set_target(current.center(rw_, rh));
            cam.set_target_zoom(current.zoom);
            cam.snap();
        }
        if (g_editor.free_camera) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) {
                cam.set_target(rw::Vec2{rw_ * 0.5f, rh * 0.5f});
                cam.set_target_zoom(1);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%.2fx  (%.0f, %.0f)", cam.zoom(), cam.center().x, cam.center().y);
        }
        ImGui::SameLine(0, 16);
        {
            const std::vector<rw::EditorTool*> tools = app.game().editor_tools();
            if (g_editor.tool_index > static_cast<int>(tools.size())) g_editor.tool_index = 0;
            const char* current = g_editor.tool_index == 0 ? "Select" : tools[static_cast<rw::usize>(g_editor.tool_index - 1)]->name();
            ImGui::SetNextItemWidth(120);
            if (ImGui::BeginCombo("##tool", current)) {
                if (ImGui::Selectable("Select", g_editor.tool_index == 0)) {
                    if (rw::EditorTool* old = active_tool(app)) old->deactivate();
                    g_editor.tool_index = 0;
                }
                for (rw::usize i = 0; i < tools.size(); ++i) {
                    if (ImGui::Selectable(tools[i]->name(), g_editor.tool_index == static_cast<int>(i) + 1)) {
                        if (rw::EditorTool* old = active_tool(app)) old->deactivate();
                        g_editor.tool_index = static_cast<int>(i) + 1;
                        tools[i]->activate();
                    }
                }
                ImGui::EndCombo();
            }
            if (rw::EditorTool* tool = active_tool(app)) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Options")) ImGui::OpenPopup("tool_options");
                if (ImGui::BeginPopup("tool_options")) {
                    tool->panel();
                    ImGui::EndPopup();
                }
            }
        }
        ImGui::SameLine(0, 16);
        ImGui::Checkbox("Gizmos", &g_editor.show_gizmos);
        if (g_editor.show_gizmos) {
            rw::GizmoSnap& snap = g_editor.gizmos.snap();
            ImGui::SameLine();
            ImGui::Checkbox("Snap", &snap.enabled);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::DragFloat("##grid", &snap.grid, 1, 1, 512, "%.0f px");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::DragFloat("##angle", &snap.angle_deg, 1, 0, 90, "%.0f deg");
            ImGui::SameLine();
            ImGui::TextDisabled("(Alt: no snap, Shift: one axis)");
        }
        ImGui::PopStyleVar();
    } else {
        ImGui::TextDisabled("no view yet");
        app.set_game_input_focus(false);
    }
    ImGui::End();
}

/// Import settings of the asset picked in the Assets panel, saved to its
/// sidecar on change; the database rescans so the asset shows as stale.
void draw_selected_asset(rw::App& app) {
    if (!g_editor.assets || g_editor.selected_asset.empty()) {
        ImGui::TextDisabled("select an asset in the Assets panel");
        return;
    }
    const rw::AssetRecord* found = g_editor.assets->find_by_path(g_editor.selected_asset);
    if (!found) {
        ImGui::TextDisabled("%s is gone (rescan)", g_editor.selected_asset.c_str());
        return;
    }
    rw::AssetRecord record = *found;
    ImGui::TextUnformatted(record.path.c_str());
    ImGui::TextDisabled("%s  %s", record.meta.type.c_str(), record.meta.id.c_str());
    if (record.meta.type == "texture") {
        if (rw::Texture* t = app.renderer().load_texture(record.path); t && t->handle) {
            const float max_w = std::max(ImGui::GetContentRegionAvail().x, 64.0f);
            const float scale = std::min(1.0f, std::min(max_w / static_cast<float>(t->width), 160.0f / static_cast<float>(t->height)));
            ImGui::Image(reinterpret_cast<ImTextureID>(t->handle), ImVec2(t->width * scale, t->height * scale));
            ImGui::TextDisabled("%d x %d", t->width, t->height);
        }
    }
    if (!record.error.empty()) ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "%s", record.error.c_str());
    else if (!record.cooked.empty()) ImGui::TextDisabled("%s", record.cooked_hash == 0 ? "not cooked" : record.stale() ? "stale, cook to update" : "cooked");

    rw::InspectorVisitor inspector;
    record.meta.reflect(inspector);
    if (inspector.changed()) {
        std::string error;
        if (g_editor.assets->save_meta(record, &error)) {
            g_editor.assets->scan();
            refresh_asset_index();
            g_editor.status = "import settings saved for " + record.path;
        } else {
            g_editor.status = error;
        }
    }
    if (!record.cooked.empty() && ImGui::Button("Cook this asset")) {
        cook_assets(false);
    }
}

void draw_assets(rw::App& app) {
    if (!g_editor.show_assets || !g_editor.assets) return;
    if (ImGui::Begin("Assets", &g_editor.show_assets)) {
        if (ImGui::Button("Rescan")) {
            g_editor.assets->scan();
            refresh_asset_index();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cook changed")) cook_assets(false);
        ImGui::SameLine();
        if (ImGui::Button("Cook all")) cook_assets(true);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_editor.project.asset_dir().string().c_str());

        const auto& records = g_editor.assets->records();
        const float row_h = 22.0f;
        if (ImGui::BeginTable("##assets", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                              ImVec2(0, 0))) {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, row_h);
            ImGui::TableSetupColumn("asset");
            ImGui::TableSetupColumn("type");
            ImGui::TableSetupColumn("id");
            ImGui::TableSetupColumn("state");
            ImGui::TableHeadersRow();
            for (const rw::AssetRecord& r : records) {
                ImGui::TableNextRow(0, row_h);
                ImGui::TableSetColumnIndex(0);
                if (r.meta.type == "texture") {
                    // Thumbnail: the renderer caches it by path, cooked or loose.
                    if (rw::Texture* t = app.renderer().load_texture(r.path); t && t->handle) {
                        const float scale = std::min(row_h / static_cast<float>(t->width), row_h / static_cast<float>(t->height));
                        ImGui::Image(reinterpret_cast<ImTextureID>(t->handle), ImVec2(t->width * scale, t->height * scale));
                    }
                }
                ImGui::TableSetColumnIndex(1);
                // Rows select (Inspector shows the import settings) and drag into asset reference fields.
                if (ImGui::Selectable((r.path + "##" + r.meta.id).c_str(), g_editor.selected_asset == r.path,
                                      ImGuiSelectableFlags_SpanAllColumns))
                    g_editor.selected_asset = r.path;
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("REWIND_ASSET", r.meta.id.c_str(), r.meta.id.size() + 1);
                    ImGui::TextUnformatted(r.path.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Copy id")) ImGui::SetClipboardText(r.meta.id.c_str());
                    if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(r.path.c_str());
                    ImGui::EndPopup();
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(r.meta.type.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextDisabled("%s", r.meta.id.c_str());
                ImGui::TableSetColumnIndex(4);
                if (!r.error.empty()) ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "%s", r.error.c_str());
                else if (r.cooked.empty()) ImGui::TextDisabled("copied as is");
                else if (r.cooked_hash == 0) ImGui::TextColored(ImVec4(1, 0.85f, 0.35f, 1), "not cooked");
                else if (r.stale()) ImGui::TextColored(ImVec4(1, 0.85f, 0.35f, 1), "stale");
                else ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1), "cooked");
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

/// Entities panel: every live entity, selection, create and destroy, prefabs.
void draw_entities(rw::App& app) {
    if (!g_editor.show_entities) return;
    rw::World* world = app.game().world();
    if (!ImGui::Begin("Entities", &g_editor.show_entities)) {
        ImGui::End();
        return;
    }
    if (!world) {
        ImGui::TextDisabled("this game exposes no world (override Game::world())");
        ImGui::End();
        return;
    }
    if (const auto link = rw::InspectorVisitor::take_link_request()) g_editor.selected = *link;

    bool edited = false;
    if (ImGui::Button("Create")) {
        g_editor.selected = world->create();
        edited = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!world->alive(g_editor.selected));
    if (ImGui::Button("Destroy")) {
        world->destroy(g_editor.selected);
        g_editor.selected = rw::Entity::null();
        edited = true;
    }
    ImGui::EndDisabled();
    if (g_editor.has_project && g_editor.prefabs.size() > 0) {
        ImGui::SameLine();
        if (ImGui::BeginCombo("##spawn", "Spawn prefab")) {
            for (const auto& [id, prefab] : g_editor.prefabs.all()) {
                if (ImGui::Selectable((prefab.name + " (" + id + ")").c_str())) {
                    g_editor.selected = rw::spawn_prefab(*world, prefab);
                    edited = true;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Text("%zu entities", world->entity_count());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filter by name, component or #index", g_editor.entity_filter,
                             sizeof g_editor.entity_filter);
    ImGui::Separator();

    const std::string filter = g_editor.entity_filter;
    const bool tree = world->has_pool<rw::Parent>() && filter.empty();
    ImGui::BeginChild("##list", ImVec2(0, 0));

    auto row_label = [&](rw::Entity e, std::string& components) {
        const std::string name = entity_name(*world, e);
        std::string line = "#" + std::to_string(e.index);
        if (!name.empty()) line += "  " + name;
        for (const std::string& c : world->component_names(e)) components += " " + c;
        return line;
    };
    // Dragging a row onto another row makes it that row's child.
    auto drag_drop = [&](rw::Entity e, bool allow_drop) {
        if (tree && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("REWIND_ENTITY", &e, sizeof e);
            ImGui::Text("#%u", e.index);
            ImGui::EndDragDropSource();
        }
        if (tree && allow_drop && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("REWIND_ENTITY")) {
                rw::Entity child;
                std::memcpy(&child, payload->Data, sizeof child);
                if (rw::set_parent(*world, child, e)) mark_edited(app);
                else g_editor.status = "cannot parent an entity below itself";
            }
            ImGui::EndDragDropTarget();
        }
    };

    if (tree) {
        std::function<void(rw::Entity)> draw_node = [&](rw::Entity e) {
            std::string components;
            const std::string line = row_label(e, components);
            const std::vector<rw::Entity> kids = rw::children_of(*world, e);
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                       ImGuiTreeNodeFlags_DefaultOpen;
            if (kids.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
            if (e == g_editor.selected) flags |= ImGuiTreeNodeFlags_Selected;
            const bool open = ImGui::TreeNodeEx((line + "##e" + std::to_string(e.index)).c_str(), flags);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) g_editor.selected = e;
            if (ImGui::IsItemHovered() && !components.empty()) ImGui::SetTooltip("%s", components.c_str() + 1);
            drag_drop(e, true);
            if (open) {
                for (rw::Entity k : kids) draw_node(k);
                ImGui::TreePop();
            }
        };
        world->each_entity([&](rw::Entity e) {
            if (rw::parent_of(*world, e).is_null()) draw_node(e);
        });
        // Drop below the tree to make an entity a root again.
        ImGui::Selectable("(drop here to unparent)", false, ImGuiSelectableFlags_Disabled);
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("REWIND_ENTITY")) {
                rw::Entity child;
                std::memcpy(&child, payload->Data, sizeof child);
                if (rw::set_parent(*world, child, rw::Entity::null())) mark_edited(app);
            }
            ImGui::EndDragDropTarget();
        }
    } else {
        world->each_entity([&](rw::Entity e) {
            std::string components;
            std::string line = row_label(e, components);
            if (!filter.empty() && line.find(filter) == std::string::npos && components.find(filter) == std::string::npos) return;
            line += "##e" + std::to_string(e.index);
            if (ImGui::Selectable(line.c_str(), e == g_editor.selected)) g_editor.selected = e;
            if (ImGui::IsItemHovered() && !components.empty()) ImGui::SetTooltip("%s", components.c_str() + 1);
            drag_drop(e, false);
        });
    }
    ImGui::EndChild();
    ImGui::End();

    if (edited) mark_edited(app);
}

/// The selected entity's components, each editable and removable, plus adding new ones.
void draw_selected_entity(rw::App& app) {
    rw::World* world = app.game().world();
    if (!world) return;
    const rw::Entity e = g_editor.selected;
    if (!world->alive(e)) {
        ImGui::TextDisabled("select an entity in the Entities panel");
        return;
    }
    ImGui::Text("entity #%u (gen %u)", e.index, e.generation);
    bool edited = false;

    // Add a component the entity does not have yet.
    if (ImGui::BeginCombo("##add", "Add component")) {
        for (const std::string& name : world->pool_names()) {
            if (world->has_component(e, name)) continue;
            if (ImGui::Selectable(name.c_str())) {
                world->add_component(e, name);
                edited = true;
            }
        }
        ImGui::EndCombo();
    }
    if (g_editor.has_project) {
        ImGui::SameLine();
        static char prefab_name[48] = "";
        ImGui::SetNextItemWidth(120);
        ImGui::InputTextWithHint("##pname", "prefab name", prefab_name, sizeof prefab_name);
        ImGui::SameLine();
        if (ImGui::Button("Make prefab") && prefab_name[0]) {
            const rw::Prefab p = rw::make_prefab(*world, e, rw::AssetDatabase::new_id(), prefab_name);
            std::error_code ec;
            std::filesystem::create_directories(g_editor.project.scene_dir() / "prefabs", ec);
            std::string error;
            const std::string file = (g_editor.project.scene_dir() / "prefabs" / (std::string(prefab_name) + ".json")).string();
            if (rw::save_prefab(file, p, &error)) {
                g_editor.status = "prefab saved: " + std::string(prefab_name);
                reload_prefabs();
            } else {
                g_editor.status = error;
            }
        }
    }

    std::string remove;
    for (const std::string& name : world->component_names(e)) {
        ImGui::PushID(name.c_str());
        const bool open = ImGui::CollapsingHeader(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
        if (ImGui::SmallButton("x")) remove = name;
        if (open) {
            rw::InspectorVisitor inspector;
            world->reflect_component(e, name, inspector);
            edited = edited || inspector.changed();
        }
        ImGui::PopID();
    }
    if (!remove.empty()) {
        world->remove_component(e, remove);
        edited = true;
    }
    if (edited) mark_edited(app);
}

void draw_panels(rw::App& app) {
    draw_entities(app);
    draw_scenes(app);
    draw_unsaved_prompt(app);

    if (g_editor.show_timeline) {
        if (ImGui::Begin("Timeline", &g_editor.show_timeline)) {
            rw::draw_timeline_panel(app.timeline(), app.time_control(), app.stats());
            ImGui::Separator();
            rw::draw_hot_reload_panel(app.hot_reload());
            if (app.stats().net) {
                ImGui::Separator();
                rw::draw_network_panel(app.stats().net);
            }
        }
        ImGui::End();
    }

    if (g_editor.show_inspector) {
        if (ImGui::Begin("Inspector", &g_editor.show_inspector)) {
            if (ImGui::CollapsingHeader("Entity", ImGuiTreeNodeFlags_DefaultOpen)) draw_selected_entity(app);
            if (g_editor.has_project && ImGui::CollapsingHeader("Asset", ImGuiTreeNodeFlags_DefaultOpen)) draw_selected_asset(app);
            if (ImGui::CollapsingHeader("Game", ImGuiTreeNodeFlags_DefaultOpen)) app.game().debug_ui();
            if (ImGui::CollapsingHeader("State")) rw::draw_state_panel(app.timeline(), app.time_control(), app.game());
        }
        ImGui::End();
    }

    if (g_editor.show_console) {
        const rw::u32 errors = rw::log_count(rw::LogLevel::Error);
        const rw::u32 warnings = rw::log_count(rw::LogLevel::Warn) - errors;
        char title[64];
        std::snprintf(title, sizeof title, "Console (%u warnings, %u errors)###Console", warnings, errors);
        if (ImGui::Begin(title, &g_editor.show_console)) rw::draw_console_panel();
        ImGui::End();
    }

    if (g_editor.show_profiler) {
        if (ImGui::Begin("Profiler", &g_editor.show_profiler)) rw::draw_profiler_panel();
        ImGui::End();
    }

    if (g_editor.show_keys) {
        if (ImGui::Begin("Keys", &g_editor.show_keys)) rw::draw_keys_panel();
        ImGui::End();
    }
    if (g_editor.show_settings) {
        if (ImGui::Begin("Player settings", &g_editor.show_settings)) rw::draw_settings_panel(app);
        ImGui::End();
    }
    if (g_editor.show_saves) {
        if (ImGui::Begin("Save games", &g_editor.show_saves)) rw::draw_saves_panel(app);
        ImGui::End();
    }

    draw_assets(app);

    if (g_editor.show_project) {
        if (ImGui::Begin("Project", &g_editor.show_project)) {
            ImGui::TextDisabled("%s", g_editor.project_file.string().c_str());
            if (g_editor.has_project) {
                ImGui::TextWrapped("Only settings that differ from what the game asks for are stored. Saved values "
                                   "apply the next time the game starts.");
            } else {
                ImGui::TextWrapped("No project: settings are saved next to the DLL and applied on the next start. "
                                   "Create a project with `rewind new` to get an asset pipeline too.");
            }
            ImGui::PushItemWidth(170);
            rw::InspectorVisitor inspector;
            g_editor.settings.reflect(inspector);
            ImGui::PopItemWidth();
            if (inspector.changed()) g_editor.settings_dirty = true;
            ImGui::Separator();
            ImGui::BeginDisabled(!g_editor.settings_dirty);
            if (ImGui::Button("Save")) save_project(app);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Revert")) {
                g_editor.settings = app.config();
                g_editor.settings_dirty = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Game defaults")) {
                g_editor.settings = g_editor.defaults;
                g_editor.settings_dirty = true;
            }
        }
        ImGui::End();
    }
}

/// One line along the bottom: mode, tick, fps, scene, selection, last message.
void draw_status_bar(rw::App& app) {
    if (!g_editor.show_status_bar) return;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = ImGui::GetFrameHeight();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##status", viewport, ImGuiDir_Down, height, flags)) {
        if (ImGui::BeginMenuBar()) {
            const rw::TimeControl& t = app.time_control();
            const rw::FrameStats& st = app.stats();
            const char* mode = t.mode == rw::TimeMode::Playing ? "PLAYING"
                             : t.mode == rw::TimeMode::Rewinding ? "REWINDING"
                             : t.mode == rw::TimeMode::Replaying ? "REPLAYING"
                             : g_editor.in_play ? "PAUSED" : "EDITING";
            const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
            ImGui::TextColored(t.mode == rw::TimeMode::Playing ? accent : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", mode);
            ImGui::Separator();
            ImGui::Text("tick %llu", static_cast<unsigned long long>(app.timeline().head()));
            ImGui::Separator();
            ImGui::Text("%.0f fps  %.1f ms", st.fps, st.frame_ms);
            ImGui::Separator();
            if (g_editor.has_project) {
                ImGui::Text("%s / %s%s", g_editor.project.name.c_str(), g_editor.scene_name.c_str(), g_editor.scene_dirty ? " *" : "");
                ImGui::Separator();
            }
            rw::World* world = app.game().world();
            if (world) {
                if (world->alive(g_editor.selected)) {
                    const std::string name = entity_name(*world, g_editor.selected);
                    ImGui::Text("#%u%s%s", g_editor.selected.index, name.empty() ? "" : "  ", name.c_str());
                } else {
                    ImGui::TextDisabled("%zu entities", world->entity_count());
                }
                ImGui::Separator();
            }
            ImGui::TextDisabled("%s", g_editor.status.c_str());
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

// ---- welcome screen ----------------------------------------------------------------------

/// A game that shows nothing, so the App can host the welcome window.
class WelcomeGame final : public rw::Game {
public:
    void configure(rw::AppConfig& cfg) override {
        cfg.title = "Rewind Editor";
        cfg.width = 900;
        cfg.height = 600;
        cfg.audio = false;
    }
    void tick(const rw::TickContext&) override {}
    void reflect(rw::Visitor&) override {}
    void render(rw::Renderer2D& r, float) override { r.clear(rw::rgb(24, 24, 28)); }
};

void welcome_ui(rw::App& app) {
    if (!g_editor.theme_applied) {
        rw::apply_editor_theme();
        g_editor.theme_applied = true;
    }
    rw::dispatch_dialog_results();
    app.time_control().show_overlay = false;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("##welcome", nullptr, flags)) {
        ImGui::SetWindowFontScale(1.6f);
        ImGui::TextUnformatted("Rewind");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("Open a project folder or a game DLL to start editing.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Open project folder...", ImVec2(190, 0)))
            rw::pick_folder([&app](const std::string& path) {
                if (path.empty()) return;
                if (!rw::is_project_dir(path)) {
                    g_editor.welcome_error = "no project.json in " + path;
                    return;
                }
                relaunch_with(app, path);
            });
        ImGui::SameLine();
        if (ImGui::Button("Open game DLL...", ImVec2(190, 0)))
            rw::pick_file([&app](const std::string& path) {
                if (!path.empty()) relaunch_with(app, path);
            }, {{"Game module", "dll;so;dylib"}});
        ImGui::SameLine();
        if (ImGui::Button("New project...", ImVec2(190, 0)))
            rw::pick_folder([&app](const std::string& dir) {
                if (dir.empty()) return;
                rw::Project project;
                std::string error;
                if (!rw::create_project(dir, fs::path(dir).filename().string(), project, &error)) {
                    g_editor.welcome_error = error;
                    return;
                }
                // A project needs its game DLL; ask for it, then open.
                rw::pick_file([&app, project](const std::string& dll) mutable {
                    if (!dll.empty()) {
                        std::error_code ec;
                        const fs::path rel = fs::relative(dll, project.root, ec);
                        project.module = (ec || rel.empty() ? fs::path(dll) : rel).generic_string();
                        std::string err;
                        rw::save_project(project, nullptr, &err);
                    }
                    relaunch_with(app, project.root.string());
                }, {{"Game module", "dll;so;dylib"}});
            });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("creates project.json, assets/, scenes/, shaders/ in a folder, then asks for the game DLL");

        ImGui::Spacing();
        ImGui::TextDisabled("Recent");
        if (g_editor.recent.empty()) ImGui::TextDisabled("nothing yet");
        for (rw::usize i = 0; i < g_editor.recent.size(); ++i) {
            const std::string& r = g_editor.recent[i];
            std::error_code ec;
            const bool exists = fs::exists(r, ec);
            ImGui::BeginDisabled(!exists);
            if (ImGui::Selectable((r + "##r" + std::to_string(i)).c_str())) relaunch_with(app, r);
            ImGui::EndDisabled();
            if (!exists && ImGui::IsItemHovered()) ImGui::SetTooltip("not found");
        }
        if (!g_editor.welcome_error.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", g_editor.welcome_error.c_str());
        }
        ImGui::Spacing();
        ImGui::TextDisabled("rewind_editor <project dir | game dll>   also works from the command line.");
    }
    ImGui::End();
}

int run_welcome(const std::string& error) {
    g_editor.welcome = true;
    g_editor.welcome_error = error;
    load_recent();
    WelcomeGame game;
    rw::AppConfig config;
    game.configure(config);
    config.start_with_overlay = false;
    std::error_code ec;
    const char* candidates[] = {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                "/System/Library/Fonts/Supplemental/Arial.ttf"};
    for (const char* c : candidates)
        if (fs::exists(c, ec)) {
            config.ui_font = c;
            config.ui_font_size = 17.0f;
            break;
        }
    rw::App app(config);
    app.set_ui_callback(welcome_ui);
    return app.run(game);
}

void editor_ui(rw::App& app) {
    const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
    if (!g_editor.layout_done) {
        default_layout(dockspace);
        g_editor.layout_done = true;
    }
    if (!g_editor.theme_applied) {
        rw::apply_editor_theme();
        g_editor.theme_applied = true;
    }
    rw::dispatch_dialog_results();
    if (g_editor.first_frame) {
        // The editor opens in edit mode; Play starts the simulation.
        app.time_control().mode = rw::TimeMode::Paused;
        g_editor.first_frame = false;
    }
    // Any way of starting the simulation (toolbar, F2, Time menu, Timeline
    // panel) counts as Play: remember the state to return to on Stop.
    const rw::TimeMode mode = app.time_control().mode;
    if (mode == rw::TimeMode::Playing && g_editor.last_mode != rw::TimeMode::Playing && !g_editor.in_play) {
        g_editor.play_start = snapshot(app);
        g_editor.in_play = true;
    }
    g_editor.last_mode = mode;
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) undo(app);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo(app);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicate_selected(app);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            if (g_editor.has_project) save_scene(app);
            else save_state(app);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !app.game_input_focus()) delete_selected(app);
    }
    draw_menu(app);
    draw_scene(app);
    draw_panels(app);
    draw_status_bar(app);
}

} // namespace

int main(int argc, char** argv) {
    std::error_code ec;
    g_editor.exe_path = fs::absolute(argv[0], ec).string();
    if (argc < 2) return run_welcome("");

    std::string module_path = argv[1];
    if (rw::is_project_dir(argv[1])) {
        std::string error;
        if (!rw::load_project(argv[1], g_editor.project, &error)) {
            std::fprintf(stderr, "rewind_editor: %s\n", error.c_str());
            return run_welcome(error);
        }
        if (g_editor.project.module.empty()) {
            const std::string msg = "project.json in " + std::string(argv[1]) + " has no \"module\": set it to the game DLL first";
            std::fprintf(stderr, "rewind_editor: %s\n", msg.c_str());
            return run_welcome(msg);
        }
        g_editor.has_project = true;
        module_path = g_editor.project.module_path().string();
        rw::use_project_paths(g_editor.project);
        g_editor.assets = std::make_unique<rw::AssetDatabase>(g_editor.project);
        g_editor.assets->scan();
        refresh_asset_index();
        refresh_scene_list();
        g_editor.project_file = g_editor.project.file();
        g_editor.state_file = g_editor.project.root / (g_editor.project.name + ".state.json");
    } else {
        const fs::path dir = fs::absolute(fs::path(argv[1])).parent_path();
        g_editor.project_file = dir / "project.json";
        g_editor.state_file = dir / (fs::path(argv[1]).stem().string() + ".state.json");
    }

    rw::GameModule module(module_path);
    if (!module.load()) {
        std::fprintf(stderr, "rewind_editor: %s\n", module.error().c_str());
        return run_welcome(module.error());
    }
    load_recent();
    remember_recent(argv[1]);

    rw::AppConfig config;
    for (int i = 2; i < argc; ++i) config.args.push_back(argv[i]);
    module.game()->configure(config);
    g_editor.defaults = config;

    if (g_editor.has_project) {
        rw::apply_project_app(g_editor.project, config);
    } else if (fs::exists(g_editor.project_file, ec)) {
        std::string error;
        if (!rw::load_json_file(g_editor.project_file.string(), config, &error))
            std::fprintf(stderr, "rewind_editor: %s\n", error.c_str());
    }
    g_editor.settings = config;

    config.embedded = true;
    config.title = "Rewind Editor  -  " + config.title;
    if (config.ui_font.empty()) {
        // A real UI font makes the editor read like a tool. Use the system's if there is one.
        const char* candidates[] = {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                    "/System/Library/Fonts/Supplemental/Arial.ttf"};
        for (const char* c : candidates)
            if (fs::exists(c, ec)) {
                config.ui_font = c;
                config.ui_font_size = 17.0f;
                break;
            }
    }
    config.width = std::max(config.width + 480, 1280);
    config.height = std::max(config.height + 320, 800);

    rw::App app(config);
    app.set_ui_callback(editor_ui);
    app.set_scene_callback(draw_scene_tools);
    if (g_editor.has_project) {
        reload_prefabs();
        app.set_start_callback([](rw::App& a) {
            if (!g_editor.project.startup_scene.empty()) load_scene_into(a, g_editor.project.startup_scene);
        });
    }
    return app.run(module);
}
