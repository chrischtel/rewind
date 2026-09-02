#include "rewind/core/file_watcher.h"

#include <algorithm>

namespace rw {

namespace fs = std::filesystem;

void FileWatcher::track(const std::string& path) {
    if (files_.contains(path)) return;
    Entry e;
    std::error_code ec;
    e.mtime = fs::last_write_time(path, ec);
    files_[path] = e;
}

void FileWatcher::untrack(const std::string& path) { files_.erase(path); }

std::vector<std::string> FileWatcher::poll(bool force) {
    std::vector<std::string> changed;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - last_check_ < std::chrono::milliseconds(check_interval_ms_)) return changed;
    last_check_ = now;

    for (auto& [path, e] : files_) {
        std::error_code ec;
        const auto mtime = fs::last_write_time(path, ec);
        if (ec) continue; // missing or being replaced: look again next time

        if (mtime == e.mtime) {
            e.pending = false;
            continue;
        }
        if (!e.pending || mtime != e.pending_mtime) {
            e.pending = true;
            e.pending_mtime = mtime;
            e.pending_since = now;
            if (settle_ms_ > 0) continue;
        }
        if (now - e.pending_since < std::chrono::milliseconds(settle_ms_)) continue;

        e.mtime = mtime;
        e.pending = false;
        changed.push_back(path);
    }
    std::sort(changed.begin(), changed.end());
    return changed;
}

} // namespace rw
