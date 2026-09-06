#pragma once

// References to assets from game state, scenes and prefabs. A reference
// stores the asset's stable id (from its .meta.json sidecar) and the path
// as a readable fallback: the id survives renames and moves, the path
// keeps files diffable and works without sidecars. Resolution goes through
// an AssetIndex, normally the global one that use_project_paths() fills.

#include "rewind/core/types.h"
#include "rewind/sim/reflect.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace rw {

struct AssetRef {
    std::string id;   ///< 16 hex digits, empty when unset
    std::string path; ///< relative to the asset root, forward slashes

    bool empty() const { return id.empty() && path.empty(); }
    void clear() {
        id.clear();
        path.clear();
    }
    bool operator==(const AssetRef&) const = default;

    void reflect(Visitor& v) {
        v.hint(Widget::Asset);
        v.field("id", id);
        v.field("path", path);
    }
};

/// Where an id lives and what it is.
struct AssetEntry {
    std::string id;
    std::string path;
    std::string type;
};

/// id <-> path lookup. Built from the sidecars in an asset folder (cheap:
/// only .meta.json files are read) or from an AssetDatabase.
class AssetIndex {
public:
    void clear();
    void add(const std::string& id, const std::string& path, const std::string& type);
    /// Read every <file>.meta.json below `asset_dir`. Returns how many were added.
    usize load_sidecars(const std::string& asset_dir);

    const AssetEntry* find_id(const std::string& id) const;
    const AssetEntry* find_path(const std::string& path) const;
    const std::vector<AssetEntry>& entries() const { return entries_; }
    usize size() const { return entries_.size(); }

    /// The path to load for a reference: by id first, then by path. Empty
    /// when unknown (with `error` set). Does not modify the reference.
    std::string resolve(const AssetRef& ref, std::string* error = nullptr) const;

    /// Bring a reference up to date: a known id refreshes the path (the
    /// file moved), a known path fills in a missing id. Returns true when
    /// the reference changed.
    bool refresh(AssetRef& ref) const;

    /// The index use_project_paths() fills and the pickers read.
    static AssetIndex& global();

private:
    std::vector<AssetEntry> entries_;
    std::unordered_map<std::string, usize> by_id_;
    std::unordered_map<std::string, usize> by_path_;
};

/// Path to load for a reference through the global index, then through the
/// asset roots (cooked variants win). Falls back to `ref.path` when the
/// index knows nothing, so games without a project keep working.
std::string resolve_asset(const AssetRef& ref);

} // namespace rw
