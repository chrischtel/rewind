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
#include <filesystem>
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
         show_project = true, show_assets = true, show_keys = false;
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
};

EditorState g_editor;

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
    world->destroy(g_editor.selected);
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
    g_editor.scene_warnings.clear();
    for (const std::string& w : warnings.items) g_editor.scene_warnings += w + "\n";
    g_editor.status = "scene loaded: " + name + (warnings.empty() ? "" : " (with warnings, see console)");
    RW_LOG_INFO("editor", "%s", g_editor.status.c_str());
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
            static char name[64] = "scene";
            ImGui::SetNextItemWidth(160);
            if (ImGui::InputText("scene name", name, sizeof name)) g_editor.scene_name = name;
            const bool has_world = app.game().world() != nullptr;
            if (ImGui::MenuItem("Save scene", nullptr, false, has_world)) save_scene(app);
            if (ImGui::MenuItem("Load scene", nullptr, false, has_world)) load_scene_into(app, g_editor.scene_name);
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
        ImGui::MenuItem("Timeline", nullptr, &g_editor.show_timeline);
        ImGui::MenuItem("Inspector", nullptr, &g_editor.show_inspector);
        ImGui::MenuItem("Assets", nullptr, &g_editor.show_assets, g_editor.has_project);
        ImGui::MenuItem("Console", nullptr, &g_editor.show_console);
        ImGui::MenuItem("Profiler", nullptr, &g_editor.show_profiler);
        ImGui::MenuItem("Project", nullptr, &g_editor.show_project);
        ImGui::MenuItem("Keys", nullptr, &g_editor.show_keys);
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
    if (!g_editor.show_scene || !g_editor.show_gizmos) return;
    rw::World* world = app.game().world();
    if (!world) return;
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
        const bool gizmo_busy = g_editor.show_gizmos && (g_editor.gizmos.dragging() || g_editor.gizmos.hot() != 0);
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

        // Right-click (without dragging) opens the placement menu.
        if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
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
            if (playing ? ImGui::Button("Pause") : ImGui::Button(g_editor.in_play ? "Resume" : "Play")) {
                if (playing) pause(app);
                else play(app);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!g_editor.in_play);
            if (ImGui::Button("Stop")) stop(app);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Step")) {
                t.mode = rw::TimeMode::Paused;
                t.step_request = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", g_editor.in_play ? (playing ? "playing" : "paused (Stop returns to the edited state)")
                                                       : "editing");
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

void draw_assets() {
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
        if (ImGui::BeginTable("##assets", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                              ImVec2(0, 0))) {
            ImGui::TableSetupColumn("asset");
            ImGui::TableSetupColumn("type");
            ImGui::TableSetupColumn("id");
            ImGui::TableSetupColumn("state");
            ImGui::TableHeadersRow();
            for (const rw::AssetRecord& r : records) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                // Rows drag into asset reference fields of the Inspector.
                ImGui::Selectable((r.path + "##" + r.meta.id).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
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
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(r.meta.type.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", r.meta.id.c_str());
                ImGui::TableSetColumnIndex(3);
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
    ImGui::BeginChild("##list", ImVec2(0, 0));
    world->each_entity([&](rw::Entity e) {
        const std::string name = entity_name(*world, e);
        std::string line = "#" + std::to_string(e.index);
        if (!name.empty()) line += "  " + name;
        std::string components;
        for (const std::string& c : world->component_names(e)) components += " " + c;
        if (!filter.empty() && line.find(filter) == std::string::npos && components.find(filter) == std::string::npos) return;
        line += "##e" + std::to_string(e.index);
        if (ImGui::Selectable(line.c_str(), e == g_editor.selected)) g_editor.selected = e;
        if (ImGui::IsItemHovered() && !components.empty()) ImGui::SetTooltip("%s", components.c_str() + 1);
    });
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

    draw_assets();

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

void editor_ui(rw::App& app) {
    const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
    if (!g_editor.layout_done) {
        default_layout(dockspace);
        g_editor.layout_done = true;
    }
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
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !app.game_input_focus()) delete_selected(app);
    }
    draw_menu(app);
    draw_scene(app);
    draw_panels(app);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rewind_editor <project dir | game dll> [game arguments...]\n");
        return 2;
    }

    std::string module_path = argv[1];
    std::error_code ec;
    if (rw::is_project_dir(argv[1])) {
        std::string error;
        if (!rw::load_project(argv[1], g_editor.project, &error)) {
            std::fprintf(stderr, "rewind_editor: %s\n", error.c_str());
            return 1;
        }
        if (g_editor.project.module.empty()) {
            std::fprintf(stderr, "rewind_editor: project.json has no \"module\"; set it to the game DLL first\n");
            return 1;
        }
        g_editor.has_project = true;
        module_path = g_editor.project.module_path().string();
        rw::use_project_paths(g_editor.project);
        g_editor.assets = std::make_unique<rw::AssetDatabase>(g_editor.project);
        g_editor.assets->scan();
        refresh_asset_index();
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
        return 1;
    }

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
