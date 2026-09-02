#pragma once

#include "rewind/core/types.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace rw {

class Game;

/// Loads a game DLL and swaps in a new build of it while the game is running.
///
/// The DLL on disk is never loaded directly: a numbered copy is made first so
/// the compiler can overwrite the original at any time. When the file changes,
/// the module waits for the write to settle, loads the new copy next to the
/// old one, hands the old game's saved state to the new game, and only then
/// unloads the old code. If anything fails, the old game keeps running.
class GameModule {
public:
    struct Reload {
        enum class Status { NoChange, Reloaded, Failed };
        Status status = Status::NoChange;
        bool state_compatible = true; ///< false: the new build could not read the old state
        std::string error;
    };

    explicit GameModule(std::filesystem::path module_path);
    ~GameModule();

    GameModule(const GameModule&) = delete;
    GameModule& operator=(const GameModule&) = delete;

    /// First load. Creates the game but does not call Game::init().
    bool load();

    /// Call once per frame. Cheap: checks the file's timestamp a few times a second.
    Reload poll();

    Game* game() const { return current_.game; }
    u32 generation() const { return generation_; }
    const std::string& error() const { return error_; }
    const std::filesystem::path& path() const { return path_; }

private:
    struct Loaded {
        void* handle = nullptr;
        Game* game = nullptr;
        Game* (*create)() = nullptr;
        void (*destroy)(Game*) = nullptr;
        std::filesystem::path copy;
    };

    bool load_copy(u32 generation, Loaded& out);
    void unload(Loaded& loaded);
    void remove_stale_copies();

    using Clock = std::chrono::steady_clock;

    std::filesystem::path path_;
    Loaded current_;
    u32 generation_ = 0;
    std::string error_;

    std::filesystem::file_time_type loaded_mtime_{};
    std::filesystem::file_time_type pending_mtime_{};
    std::filesystem::file_time_type failed_mtime_{};
    bool pending_ = false;
    bool has_failed_mtime_ = false;
    bool copy_failed_ = false;
    int copy_attempts_ = 0;
    Clock::time_point pending_since_{};
    Clock::time_point last_check_{};
};

} // namespace rw
