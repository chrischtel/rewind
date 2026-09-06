#include "rewind/project/asset_db.h"

#include "rewind/assets/aseprite.h"
#include "rewind/core/log.h"
#include "rewind/project/cooked.h"
#include "rewind/sim/serialize.h"
#include "rewind/sim/text_format.h"

#include <stb_image.h>

#include <algorithm>
#include <cstdio>
#include <random>

namespace rw {

namespace fs = std::filesystem;

namespace {

bool read_bytes(const fs::path& path, std::vector<u8>& out) {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) return false;
    out.clear();
    u8 buffer[64 * 1024];
    usize n;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) out.insert(out.end(), buffer, buffer + n);
    std::fclose(f);
    return true;
}

bool write_bytes(const fs::path& path, const std::vector<u8>& bytes, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) {
        error = "could not write " + path.string();
        return false;
    }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    if (!ok) error = "short write to " + path.string();
    return ok;
}

std::string relative_slashes(const fs::path& file, const fs::path& root) {
    std::error_code ec;
    std::string rel = fs::relative(file, root, ec).generic_string();
    return rel;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// The cook index: one entry per source path with the hash its output came from.
struct IndexEntry {
    std::string path;
    u64 hash = 0;
    void reflect(Visitor& v) {
        v.field("path", path);
        v.field("hash", hash);
    }
};

struct IndexFile {
    u32 version = 1;
    std::vector<IndexEntry> entries;
    void reflect(Visitor& v) {
        v.field("version", version);
        v.field("entries", entries);
    }
};

} // namespace

// ---- ids and types -------------------------------------------------------------------

std::string AssetDatabase::new_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(rng()));
    return buf;
}

std::string AssetDatabase::type_for(const fs::path& file) {
    const std::string ext = lower(file.extension().string());
    if (ext == ".png") return "texture";
    if (ext == ".aseprite" || ext == ".ase") return "aseprite";
    if (ext == ".wav") return "audio";
    if (ext == ".ttf" || ext == ".otf") return "font";
    if (ext == ".frag" || ext == ".vert" || ext == ".glsl") return "shader";
    if (ext == ".json") return "data";
    return "other";
}

// ---- scanning ------------------------------------------------------------------------

AssetDatabase::AssetDatabase(const Project& project) : project_(project) {}

void AssetDatabase::scan() {
    records_.clear();
    load_index();
    std::error_code ec;
    const fs::path root = project_.asset_dir();
    if (!fs::is_directory(root, ec)) return;

    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const fs::path& file = entry.path();
        const std::string name = file.filename().string();
        if (name.size() > 10 && name.compare(name.size() - 10, 10, ".meta.json") == 0) continue; // sidecar

        AssetRecord rec;
        rec.path = relative_slashes(file, root);

        // Sidecar: read it, or create it with a fresh id.
        const fs::path meta_path = file.string() + ".meta.json";
        std::string error;
        bool have_meta = fs::is_regular_file(meta_path, ec) && load_json_file(meta_path.string(), rec.meta, &error);
        if (!have_meta || rec.meta.id.empty()) {
            rec.meta.id = new_id();
            rec.meta.type = type_for(file);
            if (!save_json_file(meta_path.string(), rec.meta, &error))
                RW_LOG_WARN("assets", "could not write sidecar for %s: %s", rec.path.c_str(), error.c_str());
        }
        if (rec.meta.type.empty()) rec.meta.type = type_for(file);

        // Content hash of file bytes plus the import settings that affect the output.
        std::vector<u8> bytes;
        if (read_bytes(file, bytes)) {
            Writer settings;
            settings.pod<u8>(rec.meta.texture.nearest ? 1 : 0);
            rec.source_hash = hash_bytes(settings.bytes().data(), settings.bytes().size(), hash_bytes(bytes));
        }
        if (rec.meta.type == "texture" || rec.meta.type == "aseprite") rec.cooked = rec.path + ".cooked";
        auto it = index_.find(rec.path);
        if (it != index_.end()) rec.cooked_hash = it->second;
        if (!rec.cooked.empty() && !fs::is_regular_file(cooked_path(rec), ec)) rec.cooked_hash = 0;
        records_.push_back(std::move(rec));
    }
    std::sort(records_.begin(), records_.end(), [](const AssetRecord& a, const AssetRecord& b) { return a.path < b.path; });
}

const AssetRecord* AssetDatabase::find_by_path(const std::string& relative) const {
    for (const AssetRecord& r : records_)
        if (r.path == relative) return &r;
    return nullptr;
}

const AssetRecord* AssetDatabase::find_by_id(const std::string& id) const {
    for (const AssetRecord& r : records_)
        if (r.meta.id == id) return &r;
    return nullptr;
}

fs::path AssetDatabase::cooked_path(const AssetRecord& record) const {
    if (record.cooked.empty()) return {};
    return project_.cooked_dir() / record.cooked;
}

// ---- index ---------------------------------------------------------------------------

void AssetDatabase::load_index() {
    index_.clear();
    IndexFile file;
    std::error_code ec;
    const fs::path path = project_.cooked_dir() / "index.json";
    if (!fs::is_regular_file(path, ec)) return;
    if (!load_json_file(path.string(), file)) return;
    for (const IndexEntry& e : file.entries) index_[e.path] = e.hash;
}

void AssetDatabase::save_index() {
    IndexFile file;
    for (const AssetRecord& r : records_) {
        if (r.cooked.empty() || r.cooked_hash == 0) continue;
        file.entries.push_back(IndexEntry{r.path, r.cooked_hash});
    }
    std::error_code ec;
    fs::create_directories(project_.cooked_dir(), ec);
    std::string error;
    if (!save_json_file((project_.cooked_dir() / "index.json").string(), file, &error))
        RW_LOG_WARN("assets", "could not save the cook index: %s", error.c_str());
}

// ---- cooking -------------------------------------------------------------------------

CookReport AssetDatabase::cook(bool force, const std::function<void(const std::string&)>& progress) {
    CookReport report;
    report.scanned = static_cast<u32>(records_.size());
    for (AssetRecord& r : records_) {
        if (r.cooked.empty()) continue;
        if (!force && !r.stale()) {
            ++report.skipped;
            continue;
        }
        if (progress) progress(r.path);
        std::string error;
        if (cook_one(r, error)) {
            r.cooked_hash = r.source_hash;
            r.error.clear();
            ++report.cooked;
            report.messages.push_back("cooked " + r.path);
        } else {
            r.error = error;
            ++report.failed;
            report.messages.push_back("FAILED " + r.path + ": " + error);
            RW_LOG_ERROR("assets", "cook %s: %s", r.path.c_str(), error.c_str());
        }
    }
    save_index();
    return report;
}

bool AssetDatabase::cook_one(AssetRecord& r, std::string& error) {
    const fs::path source = project_.asset_dir() / r.path;
    std::vector<u8> bytes;
    if (!read_bytes(source, bytes)) {
        error = "could not read the source file";
        return false;
    }
    std::vector<u8> out;

    if (r.meta.type == "texture") {
        int w = 0, h = 0, n = 0;
        stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, 4);
        if (!pixels) {
            error = stbi_failure_reason();
            return false;
        }
        CookedTexture t;
        t.width = w;
        t.height = h;
        t.nearest = r.meta.texture.nearest;
        t.source_hash = r.source_hash;
        t.rgba.assign(pixels, pixels + static_cast<usize>(w) * h * 4);
        stbi_image_free(pixels);
        encode_cooked_texture(t, out);
    } else if (r.meta.type == "aseprite") {
        AseDocument doc;
        if (!parse_aseprite(bytes.data(), bytes.size(), doc, error)) return false;
        encode_cooked_sheet(cook_aseprite(doc, r.source_hash, r.meta.texture.nearest), out);
    } else {
        error = "no importer for type '" + r.meta.type + "'";
        return false;
    }
    return write_bytes(cooked_path(r), out, error);
}

} // namespace rw
