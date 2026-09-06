#include "rewind/core/paths.h"

#include "rewind/project/pack.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <filesystem>
#include <memory>
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

std::vector<std::unique_ptr<Pack>>& packs() {
    static std::vector<std::unique_ptr<Pack>> p;
    return p;
}

bool read_whole_file(const std::string& path, std::vector<unsigned char>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    unsigned char buffer[64 * 1024];
    usize n;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) out.insert(out.end(), buffer, buffer + n);
    std::fclose(f);
    return true;
}

/// Pack entry for an asset name: the cooked variant first.
const Pack* pack_with(const std::string& path, std::string& entry) {
    for (auto it = packs().rbegin(); it != packs().rend(); ++it) {
        if ((*it)->find(path + ".cooked")) {
            entry = path + ".cooked";
            return it->get();
        }
        if ((*it)->find(path)) {
            entry = path;
            return it->get();
        }
    }
    return nullptr;
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

bool add_asset_pack(const std::string& file, std::string* error) {
    auto pack = std::make_unique<Pack>();
    if (!pack->open(file, error)) return false;
    packs().push_back(std::move(pack));
    return true;
}

void clear_asset_packs() { packs().clear(); }
usize asset_pack_count() { return packs().size(); }

bool read_asset_bytes(const std::string& path, std::vector<unsigned char>& out) {
    if (path.empty()) return false;
    std::string entry;
    if (const Pack* p = pack_with(path, entry)) return p->read(entry, out);
    return read_whole_file(resolve_asset_path(path), out);
}

bool read_asset_text(const std::string& path, std::string& out) {
    std::vector<unsigned char> bytes;
    if (!read_asset_bytes(path, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

bool asset_exists(const std::string& path) {
    std::string entry;
    if (pack_with(path, entry)) return true;
    return exists(resolve_asset_path(path));
}

std::vector<std::string> packed_asset_names(const std::string& prefix) {
    std::vector<std::string> names;
    for (const auto& p : packs())
        for (const PackEntry& e : p->entries())
            if (e.name.compare(0, prefix.size(), prefix) == 0) names.push_back(e.name);
    return names;
}

} // namespace rw
