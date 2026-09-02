#pragma once

#include "rewind/core/types.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace rw {

/// Notices when tracked files change on disk. Polling, not OS notifications:
/// it checks timestamps a few times per second and reports a file only once
/// its timestamp has stopped moving, so half-written files are never read.
class FileWatcher {
public:
    void track(const std::string& path);
    void untrack(const std::string& path);
    bool tracks(const std::string& path) const { return files_.contains(path); }
    usize count() const { return files_.size(); }

    /// Paths that changed and settled since the last call. Sorted. Cheap
    /// enough to call every frame; pass `force` to skip the rate limit.
    std::vector<std::string> poll(bool force = false);

    void set_check_interval_ms(int ms) { check_interval_ms_ = ms; }
    void set_settle_ms(int ms) { settle_ms_ = ms; }

private:
    struct Entry {
        std::filesystem::file_time_type mtime{};
        std::filesystem::file_time_type pending_mtime{};
        std::chrono::steady_clock::time_point pending_since{};
        bool pending = false;
    };

    std::unordered_map<std::string, Entry> files_;
    std::chrono::steady_clock::time_point last_check_{};
    int check_interval_ms_ = 250;
    int settle_ms_ = 300;
};

} // namespace rw
