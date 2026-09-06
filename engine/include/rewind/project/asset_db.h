#pragma once

#include "rewind/core/types.h"
#include "rewind/project/project.h"
#include "rewind/sim/reflect.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rw {

/// Import settings for textures and Aseprite files, stored in the sidecar.
struct TextureImportSettings {
    bool nearest = true; ///< Sampling: nearest for pixel art, linear for smooth art.

    void reflect(Visitor& v) { v.field("nearest", nearest, {.tooltip = "nearest-neighbour sampling (pixel art)"}); }
};

/// The .meta.json next to every source asset. The id never changes once
/// assigned, so references survive renames and moves as long as the sidecar
/// moves with the file.
struct AssetMeta {
    std::string id;   ///< 16 hex digits.
    std::string type; ///< "texture", "aseprite", "audio", "font", "shader", "data", "other"
    TextureImportSettings texture;

    void reflect(Visitor& v) {
        v.field("id", id, {.read_only = true});
        v.field("type", type, {.read_only = true});
        v.field("texture", texture);
    }
};

/// One source asset as the database sees it.
struct AssetRecord {
    std::string path; ///< Relative to the asset root, forward slashes.
    AssetMeta meta;
    u64 source_hash = 0;   ///< Content hash of the file plus its import settings.
    u64 cooked_hash = 0;   ///< source_hash the current cooked output was made from, 0 if none.
    std::string cooked;    ///< Cooked output relative to the cooked root, empty if the type is not cooked.
    std::string error;     ///< Last cook error, if any.

    bool stale() const { return !cooked.empty() && cooked_hash != source_hash; }
};

struct CookReport {
    u32 scanned = 0;
    u32 cooked = 0;
    u32 skipped = 0;
    u32 failed = 0;
    std::vector<std::string> messages;
};

/// Finds every source asset in a project, keeps their sidecars and ids, and
/// cooks what changed. Staleness is decided by content hash, never by
/// timestamp, so a `git checkout` or a copy of the folder cooks exactly what
/// differs. Cooked files are written loose, mirroring the asset layout with
/// ".cooked" appended.
class AssetDatabase {
public:
    explicit AssetDatabase(const Project& project);

    /// Walk the asset root, create missing sidecars, hash every file, and
    /// read the cook index. Call before anything else and after files change.
    void scan();

    /// Cook every stale asset (or all of them with `force`). Returns what happened.
    CookReport cook(bool force = false, const std::function<void(const std::string&)>& progress = {});

    const std::vector<AssetRecord>& records() const { return records_; }
    const AssetRecord* find_by_path(const std::string& relative) const;
    const AssetRecord* find_by_id(const std::string& id) const;

    /// Cooked output path for a source path, or empty.
    std::filesystem::path cooked_path(const AssetRecord& record) const;

    /// Type by file extension, used when a sidecar is created.
    static std::string type_for(const std::filesystem::path& file);
    static std::string new_id();

private:
    bool cook_one(AssetRecord& record, std::string& error);
    void load_index();
    void save_index();

    Project project_;
    std::vector<AssetRecord> records_;
    std::map<std::string, u64> index_; ///< path -> cooked_hash from the last cook
};

} // namespace rw
