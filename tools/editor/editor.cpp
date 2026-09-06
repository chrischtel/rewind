// rewind_editor: the editor shell. Hosts a game DLL like rewind_run, but the
// game renders into a panel and the engine's tools live in dockable windows.
//
//   rewind_editor path/to/mygame_hot.dll [game arguments...]
//
// Panels: Scene (the running game; click it to give it your input), Timeline
// (transport, scrubber, determinism), Inspector (the game's debug UI and its
// whole reflected state), Console, Profiler, and Project (settings saved to
// project.json next to the DLL and applied on the next start).

#include <rewind/rewind.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct EditorState {
    fs::path project_dir;
    fs::path project_file;
    fs::path state_file;
    rw::AppConfig settings;     ///< edited in the Project panel
    bool settings_dirty = false;
    bool layout_done = false;
    bool show_scene = true, show_timeline = true, show_inspector = true, show_console = true, show_profiler = true,
         show_project = true, show_keys = false;
    std::string status;
};

EditorState g_editor;

void default_layout(ImGuiID dockspace) {
    // Scene in the middle, inspector on the right, tools along the bottom.
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspace;
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.32f, nullptr, &center);

    ImGui::DockBuilderDockWindow("Scene", center);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Project", right);
    ImGui::DockBuilderDockWindow("Timeline", bottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Profiler", bottom);
    ImGui::DockBuilderDockWindow("Keys", bottom);
    ImGui::DockBuilderFinish(dockspace);
}

void save_project(rw::App& app) {
    std::string error;
    if (rw::save_json_file(g_editor.project_file.string(), g_editor.settings, &error)) {
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
    rw::Writer w;
    app.game().save(w);
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

void draw_menu(rw::App& app) {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save project settings", nullptr, false, true)) save_project(app);
        ImGui::Separator();
        if (ImGui::MenuItem("Save state as JSON")) save_state(app);
        if (ImGui::MenuItem("Load state from JSON")) load_state(app);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Esc")) app.quit();
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
        ImGui::MenuItem("Timeline", nullptr, &g_editor.show_timeline);
        ImGui::MenuItem("Inspector", nullptr, &g_editor.show_inspector);
        ImGui::MenuItem("Console", nullptr, &g_editor.show_console);
        ImGui::MenuItem("Profiler", nullptr, &g_editor.show_profiler);
        ImGui::MenuItem("Project", nullptr, &g_editor.show_project);
        ImGui::MenuItem("Keys", nullptr, &g_editor.show_keys);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) g_editor.layout_done = false;
        ImGui::EndMenu();
    }
    const char* mode = rw::time_mode_name(app.time_control().mode);
    ImGui::SameLine(ImGui::GetWindowWidth() - 420);
    ImGui::TextDisabled("%s   tick %llu   %.0f fps   %s", mode, static_cast<unsigned long long>(app.timeline().head()),
                        app.stats().fps, g_editor.status.c_str());
    ImGui::EndMainMenuBar();
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
        app.set_game_input_focus(focused && (hovered || !ImGui::IsAnyItemActive()));
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ImGui::SetWindowFocus();
    } else {
        ImGui::TextDisabled("no view yet");
        app.set_game_input_focus(false);
    }
    ImGui::End();
}

void draw_panels(rw::App& app) {
    if (g_editor.show_timeline && ImGui::Begin("Timeline", &g_editor.show_timeline)) {
        rw::draw_timeline_panel(app.timeline(), app.time_control(), app.stats());
        ImGui::Separator();
        rw::draw_hot_reload_panel(app.hot_reload());
        if (app.stats().net) {
            ImGui::Separator();
            rw::draw_network_panel(app.stats().net);
        }
        ImGui::End();
    } else if (g_editor.show_timeline) {
        ImGui::End();
    }

    if (g_editor.show_inspector && ImGui::Begin("Inspector", &g_editor.show_inspector)) {
        if (ImGui::CollapsingHeader("Game", ImGuiTreeNodeFlags_DefaultOpen)) app.game().debug_ui();
        if (ImGui::CollapsingHeader("State", ImGuiTreeNodeFlags_DefaultOpen))
            rw::draw_state_panel(app.timeline(), app.time_control(), app.game());
        ImGui::End();
    } else if (g_editor.show_inspector) {
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

    if (g_editor.show_project) {
        if (ImGui::Begin("Project", &g_editor.show_project)) {
            ImGui::TextDisabled("%s", g_editor.project_file.string().c_str());
            ImGui::TextWrapped("Settings the game asked for, overridable per project. Saved values apply the next "
                               "time the game starts.");
            ImGui::PushItemWidth(170);
            rw::InspectorVisitor inspector;
            g_editor.settings.reflect(inspector);
            ImGui::PopItemWidth();
            if (inspector.changed()) g_editor.settings_dirty = true;
            ImGui::Separator();
            ImGui::BeginDisabled(!g_editor.settings_dirty);
            if (ImGui::Button("Save project.json")) save_project(app);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Revert")) {
                g_editor.settings = app.config();
                g_editor.settings_dirty = false;
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
    draw_menu(app);
    draw_scene(app);
    draw_panels(app);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rewind_editor <game dll> [game arguments...]\n");
        return 2;
    }

    rw::GameModule module(argv[1]);
    if (!module.load()) {
        std::fprintf(stderr, "rewind_editor: %s\n", module.error().c_str());
        return 1;
    }

    g_editor.project_dir = fs::absolute(fs::path(argv[1])).parent_path();
    g_editor.project_file = g_editor.project_dir / "project.json";
    g_editor.state_file = g_editor.project_dir / (fs::path(argv[1]).stem().string() + ".state.json");

    rw::AppConfig config;
    for (int i = 2; i < argc; ++i) config.args.push_back(argv[i]);
    module.game()->configure(config);

    // The project file overrides what the game asked for.
    std::error_code ec;
    if (fs::exists(g_editor.project_file, ec)) {
        std::string error;
        if (!rw::load_json_file(g_editor.project_file.string(), config, &error))
            std::fprintf(stderr, "rewind_editor: %s\n", error.c_str());
    }
    g_editor.settings = config;

    config.embedded = true;
    config.title = "Rewind Editor  -  " + config.title;
    config.width = std::max(config.width + 480, 1280);
    config.height = std::max(config.height + 320, 800);

    rw::App app(config);
    app.set_ui_callback(editor_ui);
    return app.run(module);
}
