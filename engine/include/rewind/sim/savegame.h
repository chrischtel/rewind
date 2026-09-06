#pragma once

// Save games: the reflected state as Game::save() writes it, behind a small
// header with a version tag, a label and a timestamp. Older saves reach
// Game::migrate_save() before loading, so a game can upgrade or refuse
// them instead of reading garbage.

#include "rewind/core/types.h"

#include <functional>
#include <string>
#include <vector>

namespace rw {

class Game;

struct SaveInfo {
    std::string file;
    std::string label;
    u32 version = 0;   ///< the game's save_version when written
    u64 timestamp = 0; ///< seconds since 1970
    u64 tick = 0;
    usize bytes = 0;   ///< state size
};

/// Upgrade a state written by an older save_version to the current one.
/// Return false to refuse the file.
using SaveMigration = std::function<bool(u32 from_version, std::vector<u8>& state)>;

bool write_savegame(const std::string& file, Game& game, u32 version, const std::string& label, u64 tick,
                    std::string* error = nullptr);
/// Reads the header, migrates when the version differs (refused when no
/// migration is given), then loads the state into the game.
bool read_savegame(const std::string& file, Game& game, u32 current_version, const SaveMigration& migrate,
                   SaveInfo* info = nullptr, std::string* error = nullptr);
/// Header only, without touching the game.
bool peek_savegame(const std::string& file, SaveInfo& info, std::string* error = nullptr);
/// Every *.rwsave in a folder, newest first.
std::vector<SaveInfo> list_savegames(const std::string& dir);
/// <user data dir>/saves/ for this organisation and title, created if missing.
std::string savegame_dir(const std::string& organization, const std::string& title);
/// Human readable timestamp, local time.
std::string format_timestamp(u64 seconds);

} // namespace rw
