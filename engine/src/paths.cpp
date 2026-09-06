#include "rewind/core/paths.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <vector>

namespace rw {

namespace {

struct Root {
    std::string dir; ///< with trailing separator
    bool cooked;
};

std::vector<Root>& roots() {
    static std::vector<Root> r;
    return r;
}

std::string with_separator(std::string dir) {
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    return dir;
}

bool exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

} // namespace

const std::string& executable_dir() {
    static const std::string dir = [] {
        const char* base = SDL_GetBasePath();
        return std::string(base ? base : "");
    }();
    return dir;
}

void add_asset_root(const std::string& dir, bool cooked) { roots().push_back(Root{with_separator(dir), cooked}); }
void clear_asset_roots() { roots().clear(); }

std::string resolve_asset_path(const std::string& path) {
    if (path.empty()) return path;
    const std::filesystem::path p(path);
    // Cooked outputs of an absolute or local source path still win.
    for (const Root& r : roots()) {
        if (!r.cooked) continue;
        if (p.is_absolute()) continue;
        const std::string candidate = r.dir + path + ".cooked";
        if (exists(candidate)) return candidate;
    }
    if (p.is_absolute() || exists(path)) return path;
    for (const Root& r : roots()) {
        if (r.cooked) continue;
        const std::string candidate = r.dir + path;
        if (exists(candidate)) return candidate;
    }
    const std::string beside = executable_dir() + path;
    if (!executable_dir().empty() && exists(beside)) return beside;
    return path;
}

} // namespace rw
