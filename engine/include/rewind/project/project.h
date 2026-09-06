#pragma once

#include "rewind/core/app.h"
#include "rewind/core/types.h"
#include "rewind/sim/reflect.h"

#include <filesystem>
#include <string>

namespace rw {

/// A project is a folder with a fixed layout:
///
///     project.json        this file
///     assets/             source assets, each with a .meta.json sidecar
///     scenes/             scene and prefab files (JSON)
///     shaders/            GLSL materials
///     build/cooked/       cooked assets, one file per source, regenerated
///
/// Everything under build/ is derived and safe to delete.
struct Project {
    std::string name = "untitled";
    std::string module;       ///< Game DLL relative to the project root, e.g. "build/bin/Release/game_hot.dll".
    std::string startup_scene; ///< Relative to scenes/, may be empty.
    u32 format_version = 1;
    AppConfig app;            ///< Window, rendering, timeline and UI settings.

    void reflect(Visitor& v) {
        v.field("name", name);
        v.field("module", module, {.tooltip = "game DLL, relative to the project folder"});
        v.field("startup_scene", startup_scene);
        v.field("format_version", format_version, {.read_only = true});
        v.field("app", app);
    }

    // Layout, resolved from the root the project was loaded from.
    std::filesystem::path root;
    std::filesystem::path file() const { return root / "project.json"; }
    std::filesystem::path asset_dir() const { return root / "assets"; }
    std::filesystem::path scene_dir() const { return root / "scenes"; }
    std::filesystem::path shader_dir() const { return root / "shaders"; }
    std::filesystem::path build_dir() const { return root / "build"; }
    std::filesystem::path cooked_dir() const { return root / "build" / "cooked"; }
    std::filesystem::path module_path() const { return module.empty() ? std::filesystem::path() : root / module; }
};

/// True if `dir` holds a project.json.
bool is_project_dir(const std::filesystem::path& dir);

/// Read <dir>/project.json. Unknown fields are ignored, missing ones keep defaults.
bool load_project(const std::filesystem::path& dir, Project& out, std::string* error = nullptr);

/// Write project.json. With `app_defaults` (what the game's configure() asks
/// for), only the app settings that differ from them are stored, so a game's
/// own defaults keep applying to everything the project never overrode.
bool save_project(const Project& project, const AppConfig* app_defaults = nullptr, std::string* error = nullptr);

/// Apply the project file's app overrides on top of `config` (typically the
/// result of Game::configure()).
bool apply_project_app(const Project& project, AppConfig& config, std::string* error = nullptr);

/// Create the folder layout, a project.json, and a small starter asset so the
/// pipeline has something to cook. Fails if project.json already exists.
bool create_project(const std::filesystem::path& dir, const std::string& name, Project& out,
                    std::string* error = nullptr);

/// Point the engine's asset path resolution at this project: cooked files
/// win, then source assets, then the usual fallbacks.
void use_project_paths(const Project& project);

} // namespace rw
