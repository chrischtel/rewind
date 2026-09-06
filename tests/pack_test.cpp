// Headless checks for asset packs: write, open, read, priority over loose
// files, packed names, damaged files.

#include <rewind/core/paths.h>
#include <rewind/project/pack.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

std::vector<u8> bytes_of(const std::string& s) { return std::vector<u8>(s.begin(), s.end()); }

void test_pack() {
    const fs::path dir = fs::temp_directory_path() / "rewind_pack_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "loose" / "sprites");
    const std::string file = (dir / "game.rwpak").string();

    std::vector<PackInput> inputs;
    inputs.push_back({"sprites/hero.png.cooked", bytes_of("cooked hero")});
    inputs.push_back({"fonts/ui.ttf", bytes_of("font bytes")});
    inputs.push_back({"scenes/level1.json", bytes_of("{}")});
    inputs.push_back({"scenes/prefabs/tree.json", bytes_of("{\"tree\":1}")});
    inputs.push_back({"empty.bin", {}});
    std::string error;
    CHECK(write_pack(file, inputs, &error));

    Pack pack;
    CHECK(pack.open(file, &error));
    CHECK(pack.entries().size() == 5);
    CHECK(pack.find("fonts/ui.ttf") && pack.find("fonts/ui.ttf")->size == 10);
    CHECK(!pack.find("nope"));
    std::vector<u8> out;
    CHECK(pack.read("sprites/hero.png.cooked", out) && out == bytes_of("cooked hero"));
    CHECK(pack.read("scenes/prefabs/tree.json", out) && out == bytes_of("{\"tree\":1}"));
    CHECK(pack.read("empty.bin", out) && out.empty());
    CHECK(!pack.read("missing", out));
    // Hashes are content hashes: equal content, equal hash.
    CHECK(pack.find("scenes/level1.json")->hash != pack.find("fonts/ui.ttf")->hash);

    // Mounted: the loader sees the pack before the loose file, cooked name first.
    {
        std::ofstream loose(dir / "loose" / "sprites" / "hero.png", std::ios::binary);
        loose << "loose hero";
    }
    clear_asset_roots();
    clear_asset_packs();
    add_asset_root((dir / "loose").string(), false);
    CHECK(read_asset_bytes("sprites/hero.png", out) && out == bytes_of("loose hero"));
    CHECK(add_asset_pack(file, &error));
    CHECK(asset_pack_count() == 1);
    CHECK(read_asset_bytes("sprites/hero.png", out) && out == bytes_of("cooked hero"));
    CHECK(read_asset_bytes("fonts/ui.ttf", out) && out == bytes_of("font bytes"));
    std::string text;
    CHECK(read_asset_text("scenes/level1.json", text) && text == "{}");
    CHECK(asset_exists("fonts/ui.ttf") && !asset_exists("fonts/none.ttf"));
    CHECK(!read_asset_bytes("nothing/here", out));
    const std::vector<std::string> prefabs = packed_asset_names("scenes/prefabs/");
    CHECK(prefabs.size() == 1 && prefabs[0] == "scenes/prefabs/tree.json");
    // Absolute paths still read straight from disk.
    CHECK(read_asset_bytes((dir / "loose" / "sprites" / "hero.png").string(), out) && out == bytes_of("loose hero"));
    clear_asset_packs();
    clear_asset_roots();

    // Not a pack, and a truncated pack, are refused without crashing.
    Pack bad;
    CHECK(!bad.open((dir / "loose" / "sprites" / "hero.png").string(), &error) && !error.empty());
    CHECK(!bad.open((dir / "does_not_exist.rwpak").string(), &error));
    {
        std::vector<u8> all;
        std::ifstream in(file, std::ios::binary);
        all.assign(std::istreambuf_iterator<char>(in), {});
        all.resize(all.size() / 3);
        std::ofstream cut(dir / "cut.rwpak", std::ios::binary);
        cut.write(reinterpret_cast<const char*>(all.data()), static_cast<std::streamsize>(all.size()));
    }
    CHECK(!bad.open((dir / "cut.rwpak").string(), &error));
    pack.close();
    fs::remove_all(dir);
}

} // namespace

int main() {
    test_pack();
    if (failures == 0) std::printf("pack_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
