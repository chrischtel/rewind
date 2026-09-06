#include "rewind/project/asset_ref.h"

#include "rewind/core/log.h"
#include "rewind/core/paths.h"
#include "rewind/project/asset_db.h"
#include "rewind/sim/text_format.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace rw {

void AssetIndex::clear() {
    entries_.clear();
    by_id_.clear();
    by_path_.clear();
}

void AssetIndex::add(const std::string& id, const std::string& path, const std::string& type) {
    auto it = by_id_.find(id);
    if (it != by_id_.end()) {
        AssetEntry& e = entries_[it->second];
        by_path_.erase(e.path);
        e.path = path;
        e.type = type;
        by_path_[path] = it->second;
        return;
    }
    entries_.push_back(AssetEntry{id, path, type});
    by_id_[id] = entries_.size() - 1;
    by_path_[path] = entries_.size() - 1;
}

usize AssetIndex::load_sidecars(const std::string& asset_dir) {
    std::error_code ec;
    if (!fs::is_directory(asset_dir, ec)) return 0;
    usize added = 0;
    const std::string suffix = ".meta.json";
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(asset_dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        AssetMeta meta;
        std::string error;
        if (!load_json_file(entry.path().string(), meta, &error) || meta.id.empty()) continue;
        const fs::path source = entry.path().parent_path() / name.substr(0, name.size() - suffix.size());
        std::string rel = fs::relative(source, asset_dir, ec).generic_string();
        if (ec || rel.empty()) continue;
        add(meta.id, rel, meta.type);
        ++added;
    }
    return added;
}

const AssetEntry* AssetIndex::find_id(const std::string& id) const {
    auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : &entries_[it->second];
}

const AssetEntry* AssetIndex::find_path(const std::string& path) const {
    auto it = by_path_.find(path);
    return it == by_path_.end() ? nullptr : &entries_[it->second];
}

std::string AssetIndex::resolve(const AssetRef& ref, std::string* error) const {
    if (!ref.id.empty())
        if (const AssetEntry* e = find_id(ref.id)) return e->path;
    if (!ref.path.empty())
        if (const AssetEntry* e = find_path(ref.path)) return e->path;
    if (error) {
        if (ref.empty()) *error = "empty asset reference";
        else *error = "unknown asset: id '" + ref.id + "' path '" + ref.path + "'";
    }
    return {};
}

bool AssetIndex::refresh(AssetRef& ref) const {
    if (!ref.id.empty()) {
        if (const AssetEntry* e = find_id(ref.id)) {
            if (e->path == ref.path) return false;
            ref.path = e->path;
            return true;
        }
    }
    if (!ref.path.empty()) {
        if (const AssetEntry* e = find_path(ref.path)) {
            if (e->id == ref.id) return false;
            ref.id = e->id;
            return true;
        }
    }
    return false;
}

AssetIndex& AssetIndex::global() {
    static AssetIndex index;
    return index;
}

std::string resolve_asset(const AssetRef& ref) {
    std::string error;
    std::string path = AssetIndex::global().resolve(ref, &error);
    if (path.empty()) {
        if (ref.path.empty()) return {};
        path = ref.path; // no index (no project): trust the path
    }
    return resolve_asset_path(path);
}

} // namespace rw
