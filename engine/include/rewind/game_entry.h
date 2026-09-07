#pragma once

// End your game's source file with REWIND_GAME(YourGameType).
//
// Built as an executable it expands to main(). Built with REWIND_GAME_DLL
// (which rewind_add_game() does for the `<name>_hot` target) it expands to
// the entry points that rewind_run and GameModule look for.

#include "rewind/core/app.h"
#include "rewind/core/paths.h"
#include "rewind/core/types.h"
#include "rewind/sim/game.h"

namespace rw {
constexpr u32 kGameApiVersion = 1;

/// rewind_add_game(... ASSETS dir): that folder is an asset root while developing.
inline void add_game_assets_root() {
#if defined(REWIND_GAME_ASSETS)
    for (const std::string& root : asset_roots())
        if (root == REWIND_GAME_ASSETS) return;
    add_asset_root(REWIND_GAME_ASSETS, false);
#endif
}
}

#if defined(_WIN32)
#define REWIND_EXPORT extern "C" __declspec(dllexport)
#else
#define REWIND_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#if defined(REWIND_GAME_DLL)

#define REWIND_GAME(GameType)                                                                  \
    REWIND_EXPORT rw::u32 rewind_game_api_version() { return rw::kGameApiVersion; }            \
    REWIND_EXPORT rw::Game* rewind_create_game() {                                            \
        rw::add_game_assets_root();                                                            \
        return new GameType();                                                                 \
    }                    \
    REWIND_EXPORT void rewind_destroy_game(rw::Game* game) { delete game; }

#else

#define REWIND_GAME(GameType)                                                                  \
    int main(int argc, char** argv) {                                                          \
        rw::add_game_assets_root();                                                            \
        GameType game;                                                                         \
        rw::AppConfig config;                                                                  \
        for (int i = 1; i < argc; ++i) config.args.push_back(argv[i]);                         \
        game.configure(config);                                                                \
        return rw::App(config).run(game);                                                      \
    }

#endif
