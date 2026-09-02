// rewind_run: hosts a game DLL and reloads it every time it is rebuilt.
//
//   rewind_run path/to/mygame_hot.dll
//
// Keep it running, edit your game, rebuild only the game target, and the new
// code takes over with the simulation state and the whole timeline intact.

#include <rewind/core/app.h>
#include <rewind/core/game_module.h>
#include <rewind/sim/game.h>

#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rewind_run <game dll> [game arguments...]\n");
        return 2;
    }

    rw::GameModule module(argv[1]);
    if (!module.load()) {
        std::fprintf(stderr, "rewind_run: %s\n", module.error().c_str());
        return 1;
    }

    rw::AppConfig config;
    for (int i = 2; i < argc; ++i) config.args.push_back(argv[i]);
    module.game()->configure(config);

    rw::App app(config);
    return app.run(module);
}
