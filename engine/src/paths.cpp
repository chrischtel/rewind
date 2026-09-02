#include "rewind/core/paths.h"

#include <SDL3/SDL.h>

#include <filesystem>

namespace rw {

const std::string& executable_dir() {
    static const std::string dir = [] {
        const char* base = SDL_GetBasePath();
        return std::string(base ? base : "");
    }();
    return dir;
}

std::string resolve_asset_path(const std::string& path) {
    if (path.empty()) return path;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.is_absolute() || std::filesystem::exists(p, ec)) return path;
    const std::string beside = executable_dir() + path;
    if (!executable_dir().empty() && std::filesystem::exists(beside, ec)) return beside;
    return path;
}

} // namespace rw
