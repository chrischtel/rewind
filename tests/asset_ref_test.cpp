// Headless checks for asset references: index from sidecars, resolve by id
// and path, refresh after a move, reflection, and the global fallback.

#include <rewind/core/paths.h>
#include <rewind/project/asset_db.h>
#include <rewind/project/asset_ref.h>
#include <rewind/sim/text_format.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace rw;
namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

void write_file(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

void write_sidecar(const fs::path& file, const std::string& id, const std::string& type) {
    AssetMeta meta;
    meta.id = id;
    meta.type = type;
    std::string error;
    CHECK(save_json_file(file.string() + ".meta.json", meta, &error));
}

void test_index_and_resolve() {
    const fs::path dir = fs::temp_directory_path() / "rewind_asset_ref_test";
    fs::remove_all(dir);
    write_file(dir / "sprites" / "hero.png", "png");
    write_sidecar(dir / "sprites" / "hero.png", "00000000000000a1", "texture");
    write_file(dir / "sfx" / "jump.wav", "wav");
    write_sidecar(dir / "sfx" / "jump.wav", "00000000000000b2", "audio");
    write_file(dir / "notes.txt", "no sidecar");

    AssetIndex index;
    CHECK(index.load_sidecars(dir.string()) == 2);
    CHECK(index.size() == 2);
    CHECK(index.find_id("00000000000000a1") && index.find_id("00000000000000a1")->path == "sprites/hero.png");
    CHECK(index.find_path("sfx/jump.wav") && index.find_path("sfx/jump.wav")->type == "audio");
    CHECK(!index.find_id("nope"));

    // Resolve by id wins over a stale path; by path works without an id.
    AssetRef by_id{"00000000000000a1", "old/location.png"};
    CHECK(index.resolve(by_id) == "sprites/hero.png");
    AssetRef by_path{"", "sfx/jump.wav"};
    CHECK(index.resolve(by_path) == "sfx/jump.wav");
    std::string error;
    CHECK(index.resolve(AssetRef{"ffff", "missing.png"}, &error).empty() && !error.empty());
    CHECK(index.resolve(AssetRef{}, &error).empty());

    // refresh() updates what it can and reports whether it did.
    CHECK(index.refresh(by_id) && by_id.path == "sprites/hero.png");
    CHECK(!index.refresh(by_id));
    CHECK(index.refresh(by_path) && by_path.id == "00000000000000b2");
    AssetRef unknown{"ffff", "missing.png"};
    CHECK(!index.refresh(unknown) && unknown.path == "missing.png");

    // A moved file: the sidecar moves with it, the id resolves to the new place.
    fs::create_directories(dir / "art");
    fs::rename(dir / "sprites" / "hero.png", dir / "art" / "hero.png");
    fs::rename(dir / "sprites" / "hero.png.meta.json", dir / "art" / "hero.png.meta.json");
    AssetIndex after;
    after.load_sidecars(dir.string());
    CHECK(after.resolve(AssetRef{"00000000000000a1", "sprites/hero.png"}) == "art/hero.png");

    // Global resolution: index + asset roots give a loadable file; without
    // an index entry the path is trusted as is.
    AssetIndex::global().clear();
    AssetIndex::global().load_sidecars(dir.string());
    clear_asset_roots();
    add_asset_root(dir.string(), false);
    const std::string file = resolve_asset(AssetRef{"00000000000000a1", ""});
    CHECK(fs::exists(file) && fs::path(file).filename() == "hero.png");
    CHECK(resolve_asset(AssetRef{"", "notes.txt"}).size() > 0);
    CHECK(resolve_asset(AssetRef{}).empty());
    AssetIndex::global().clear();
    clear_asset_roots();

    // The reference reflects to JSON as id + path.
    AssetRef ref{"00000000000000a1", "art/hero.png"};
    const std::string json = to_json(ref);
    CHECK(json.find("00000000000000a1") != std::string::npos && json.find("art/hero.png") != std::string::npos);
    AssetRef back;
    CHECK(from_json(json, back, &error) && back == ref);

    fs::remove_all(dir);
}

} // namespace

int main() {
    test_index_and_resolve();
    if (failures == 0) std::printf("asset_ref_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
