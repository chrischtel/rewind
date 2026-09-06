// rewind_run: hosts a game DLL and reloads it every time it is rebuilt.
//
//   rewind_run path/to/mygame_hot.dll [game arguments...]
//   rewind_run path/to/project        (uses the project's module and assets)
//
// Keep it running, edit your game, rebuild only the game target, and the new
// code takes over with the simulation state and the whole timeline intact.

#include <rewind/core/app.h>
#include <rewind/core/game_module.h>
#include <rewind/project/project.h>
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
    const bool has_project = rw::is_project_dir(argv[1]);
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
    return app.run(module);
}
