// rewind_run: hosts a game DLL and reloads it every time it is rebuilt.
//
//   rewind_run path/to/mygame_hot.dll [game arguments...]
//   rewind_run path/to/project        (uses the project's module and assets)
//
// Keep it running, edit your game, rebuild only the game target, and the new
// code takes over with the simulation state and the whole timeline intact.

#include <rewind/core/app.h>
#include <rewind/core/game_module.h>
#include <rewind/core/paths.h>
#include <rewind/sim/text_format.h>
#include <rewind/project/project.h>
#include <rewind/project/scene.h>
#include <rewind/sim/game.h>

#include <cstdio>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rewind_run <game dll | project dir> [game arguments...]\n");
        return 2;
    }

    std::string module_path = argv[1];
    rw::Project project;
    bool has_project = rw::is_project_dir(argv[1]);
    if (has_project) {
        std::string error;
        if (!rw::load_project(argv[1], project, &error)) {
            std::fprintf(stderr, "rewind_run: %s\n", error.c_str());
            return 1;
        }
        if (project.module.empty()) {
            std::fprintf(stderr, "rewind_run: project.json has no \"module\"\n");
            return 1;
        }
        module_path = project.module_path().string();
        rw::use_project_paths(project);
    }

    // A shipped game: every .rwpak next to the executable or the DLL is mounted,
    // and project.json inside the first one supplies settings and the startup scene.
    bool packed = false;
    if (!has_project) {
        std::error_code ec;
        for (const std::filesystem::path dir : {std::filesystem::path(rw::executable_dir()),
                                                std::filesystem::absolute(module_path, ec).parent_path()}) {
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file(ec) || entry.path().extension() != ".rwpak") continue;
                std::string error;
                if (rw::add_asset_pack(entry.path().string(), &error)) packed = true;
                else std::fprintf(stderr, "rewind_run: %s\n", error.c_str());
            }
            if (packed) break;
        }
        std::string text;
        if (packed && rw::read_asset_text("project.json", text)) {
            std::string error;
            if (rw::from_json(text, project, &error)) has_project = true;
            else std::fprintf(stderr, "rewind_run: packed project.json: %s\n", error.c_str());
        }
    }

    rw::GameModule module(module_path);
    if (!module.load()) {
        std::fprintf(stderr, "rewind_run: %s\n", module.error().c_str());
        return 1;
    }

    rw::AppConfig config;
    for (int i = 2; i < argc; ++i) config.args.push_back(argv[i]);
    module.game()->configure(config);
    if (has_project) rw::apply_project_app(project, config);

    rw::App app(config);
    if (has_project && !project.startup_scene.empty()) {
        app.set_start_callback([&project, packed](rw::App& a) {
            rw::World* world = a.game().world();
            if (!world) return;
            std::string file = project.startup_scene;
            if (file.size() < 5 || file.compare(file.size() - 5, 5, ".json") != 0) file += ".json";
            rw::Scene scene;
            std::string error;
            const std::string scene_file = packed ? "scenes/" + file : (project.scene_dir() / file).string();
            if (!rw::load_scene(scene_file, scene, &error)) {
                std::fprintf(stderr, "rewind_run: startup scene: %s\n", error.c_str());
                return;
            }
            rw::PrefabLibrary prefabs;
            if (packed) prefabs.load_packed();
            else prefabs.load_dir((project.scene_dir() / "prefabs").string());
            rw::apply_scene(scene, *world, &prefabs);
        });
    }
    return app.run(module);
}
