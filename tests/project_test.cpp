// Headless checks for the project layer: layout, project file, sidecars with
// stable ids, content-hash cooking, cooked formats, and path resolution.

#include <rewind/assets/png.h>
#include <rewind/core/paths.h>
#include <rewind/project/asset_db.h>
#include <rewind/project/cooked.h>
#include <rewind/project/project.h>
#include <rewind/sim/text_format.h>

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

std::vector<u8> read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_all(const fs::path& p, const std::vector<u8>& bytes) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A minimal RGBA .aseprite writer: one layer, N raw-cel frames, one tag.
struct Bytes {
    std::vector<u8> v;
    void b(u8 x) { v.push_back(x); }
    void w(u16 x) { b(static_cast<u8>(x)); b(static_cast<u8>(x >> 8)); }
    void d(u32 x) { w(static_cast<u16>(x)); w(static_cast<u16>(x >> 16)); }
    void s(const std::string& t) { w(static_cast<u16>(t.size())); for (char c : t) b(static_cast<u8>(c)); }
    void raw(const std::vector<u8>& x) { v.insert(v.end(), x.begin(), x.end()); }
    void patch(usize at, u32 x) { v[at] = static_cast<u8>(x); v[at + 1] = static_cast<u8>(x >> 8); v[at + 2] = static_cast<u8>(x >> 16); v[at + 3] = static_cast<u8>(x >> 24); }
};

std::vector<u8> make_ase(int w, int h, const std::vector<std::vector<u8>>& frames) {
    Bytes f;
    f.d(0); f.w(0xA5E0); f.w(static_cast<u16>(frames.size())); f.w(static_cast<u16>(w)); f.w(static_cast<u16>(h)); f.w(32);
    f.d(1); f.w(100); f.d(0); f.d(0); f.b(0); f.b(0); f.b(0); f.b(0); f.w(256); f.b(1); f.b(1);
    f.w(0); f.w(0); f.w(16); f.w(16);
    for (int i = 0; i < 84; ++i) f.b(0);
    for (usize i = 0; i < frames.size(); ++i) {
        const usize start = f.v.size();
        const int chunks = i == 0 ? 3 : 1;
        f.d(0); f.w(0xF1FA); f.w(static_cast<u16>(chunks)); f.w(120); f.b(0); f.b(0); f.d(static_cast<u32>(chunks));
        if (i == 0) {
            Bytes layer; layer.w(1); layer.w(0); layer.w(0); layer.w(0); layer.w(0); layer.w(0); layer.b(255); layer.b(0); layer.b(0); layer.b(0); layer.s("base");
            f.d(static_cast<u32>(layer.v.size() + 6)); f.w(0x2004); f.raw(layer.v);
            Bytes tags; tags.w(1); for (int k = 0; k < 8; ++k) tags.b(0);
            tags.w(0); tags.w(static_cast<u16>(frames.size() - 1)); tags.b(0); tags.w(0); for (int k = 0; k < 6; ++k) tags.b(0); tags.b(0); tags.b(0); tags.b(0); tags.b(0); tags.s("loop");
            f.d(static_cast<u32>(tags.v.size() + 6)); f.w(0x2018); f.raw(tags.v);
        }
        Bytes cel; cel.w(0); cel.w(0); cel.w(0); cel.b(255); cel.w(0); cel.w(0); for (int k = 0; k < 5; ++k) cel.b(0);
        cel.w(static_cast<u16>(w)); cel.w(static_cast<u16>(h)); cel.raw(frames[i]);
        f.d(static_cast<u32>(cel.v.size() + 6)); f.w(0x2005); f.raw(cel.v);
        f.patch(start, static_cast<u32>(f.v.size() - start));
    }
    f.patch(0, static_cast<u32>(f.v.size()));
    return f.v;
}

void test_create_and_load() {
    const fs::path dir = fs::temp_directory_path() / "rewind_project_test";
    std::error_code ec;
    fs::remove_all(dir, ec);

    Project created;
    std::string error;
    CHECK(create_project(dir, "Demo", created, &error));
    CHECK(is_project_dir(dir));
    CHECK(fs::is_directory(dir / "assets") && fs::is_directory(dir / "scenes") && fs::is_directory(dir / "shaders"));
    CHECK(fs::is_directory(dir / "build" / "cooked"));
    CHECK(fs::is_regular_file(dir / "assets" / "coin.png"));

    Project loaded;
    CHECK(load_project(dir, loaded, &error));
    CHECK(loaded.name == "Demo" && loaded.app.title == "Demo");
    CHECK(loaded.root == fs::absolute(dir));
    CHECK(loaded.module_path().empty());

    loaded.module = "build/bin/Release/demo_hot.dll";
    loaded.app.timeline.tick_rate = 30;
    CHECK(save_project(loaded, nullptr, &error));
    Project again;
    CHECK(load_project(dir, again, &error));
    CHECK(again.module == "build/bin/Release/demo_hot.dll" && again.app.timeline.tick_rate == 30);
    CHECK(again.module_path() == fs::absolute(dir) / "build/bin/Release/demo_hot.dll");

    // Saving against the game's defaults stores only what differs, and applying
    // the file on top of a game's configuration keeps everything else.
    AppConfig game_defaults;
    game_defaults.render_width = 320;
    game_defaults.render_height = 180;
    game_defaults.timeline.history_ticks = 999;
    Project overrides = again;
    overrides.app = game_defaults;
    overrides.app.timeline.tick_rate = 30; // the one thing the project changes
    CHECK(save_project(overrides, &game_defaults, &error));
    const std::string text = read_text(dir / "project.json");
    CHECK(text.find("tick_rate") != std::string::npos);
    CHECK(text.find("render_width") == std::string::npos);
    AppConfig config = game_defaults;
    CHECK(apply_project_app(again, config, &error));
    CHECK(config.timeline.tick_rate == 30);
    CHECK(config.render_width == 320 && config.timeline.history_ticks == 999);

    // Creating on top of an existing project is refused.
    Project dup;
    CHECK(!create_project(dir, "Dup", dup, &error));
    CHECK(!error.empty());
}

void test_sidecars_and_cooking() {
    const fs::path dir = fs::temp_directory_path() / "rewind_project_test";
    Project project;
    std::string error;
    CHECK(load_project(dir, project, &error));

    // A second texture and an Aseprite file in a subfolder.
    std::vector<u8> px(8 * 8 * 4, 200);
    fs::create_directories(dir / "assets" / "sprites");
    CHECK(write_png((dir / "assets" / "sprites" / "block.png").string(), 8, 8, px.data(), &error));
    std::vector<u8> red(4 * 4 * 4), blue(4 * 4 * 4);
    for (usize i = 0; i < 16; ++i) { red[i * 4] = 255; red[i * 4 + 3] = 255; blue[i * 4 + 2] = 255; blue[i * 4 + 3] = 255; }
    write_all(dir / "assets" / "sprites" / "hero.aseprite", make_ase(4, 4, {red, blue}));
    write_all(dir / "assets" / "notes.txt", {'h', 'i'});

    AssetDatabase db(project);
    db.scan();
    CHECK(db.records().size() == 4);
    const AssetRecord* coin = db.find_by_path("coin.png");
    const AssetRecord* block = db.find_by_path("sprites/block.png");
    const AssetRecord* hero = db.find_by_path("sprites/hero.aseprite");
    const AssetRecord* notes = db.find_by_path("notes.txt");
    CHECK(coin && block && hero && notes);
    CHECK(coin->meta.type == "texture" && hero->meta.type == "aseprite" && notes->meta.type == "other");
    CHECK(coin->meta.id.size() == 16 && coin->meta.id != block->meta.id);
    CHECK(fs::is_regular_file(dir / "assets" / "coin.png.meta.json"));
    CHECK(coin->cooked == "coin.png.cooked" && notes->cooked.empty());
    CHECK(coin->cooked_hash == 0 && coin->stale());
    const std::string coin_id = coin->meta.id;

    // First cook: the three cookable assets.
    CookReport report = db.cook();
    CHECK(report.cooked == 3 && report.failed == 0 && report.skipped == 0);
    CHECK(fs::is_regular_file(dir / "build" / "cooked" / "coin.png.cooked"));
    CHECK(fs::is_regular_file(dir / "build" / "cooked" / "sprites" / "hero.aseprite.cooked"));
    CHECK(fs::is_regular_file(dir / "build" / "cooked" / "index.json"));

    // Cooked texture decodes to the source pixels.
    CookedTexture tex;
    CHECK(decode_cooked_texture(read_all(dir / "build" / "cooked" / "sprites" / "block.png.cooked"), tex));
    CHECK(tex.width == 8 && tex.height == 8 && tex.rgba == px && tex.nearest);

    // Cooked sheet carries frames, durations and tags.
    CookedSheet sheet;
    CHECK(decode_cooked_sheet(read_all(dir / "build" / "cooked" / "sprites" / "hero.aseprite.cooked"), sheet));
    CHECK(sheet.frame_width == 4 && sheet.frame_height == 4 && sheet.durations_ms.size() == 2);
    CHECK(sheet.durations_ms[0] == 120);
    CHECK(sheet.tags.size() == 1 && sheet.tags[0].name == "loop" && sheet.tags[0].to == 1);
    CHECK(sheet.columns == 2 && sheet.atlas_width == 8 && sheet.atlas_height == 4);
    CHECK(sheet.rgba[0] == 255 && sheet.rgba[(4) * 4 + 2] == 255); // frame 0 red, frame 1 blue

    // Second scan and cook: ids are stable, nothing to do.
    AssetDatabase db2(project);
    db2.scan();
    CHECK(db2.find_by_path("coin.png")->meta.id == coin_id);
    CHECK(!db2.find_by_path("coin.png")->stale());
    report = db2.cook();
    CHECK(report.cooked == 0 && report.skipped == 3);

    // Changing a file's content makes only it stale.
    px[0] = 1;
    CHECK(write_png((dir / "assets" / "sprites" / "block.png").string(), 8, 8, px.data(), &error));
    AssetDatabase db3(project);
    db3.scan();
    CHECK(db3.find_by_path("sprites/block.png")->stale());
    CHECK(!db3.find_by_path("coin.png")->stale());
    report = db3.cook();
    CHECK(report.cooked == 1 && report.skipped == 2);

    // Changing import settings in the sidecar also counts as a change.
    AssetMeta meta;
    CHECK(load_json_file((dir / "assets" / "coin.png.meta.json").string(), meta, &error));
    meta.texture.nearest = false;
    CHECK(save_json_file((dir / "assets" / "coin.png.meta.json").string(), meta, &error));
    AssetDatabase db4(project);
    db4.scan();
    CHECK(db4.find_by_path("coin.png")->stale());
    report = db4.cook();
    CHECK(report.cooked == 1);
    CHECK(decode_cooked_texture(read_all(dir / "build" / "cooked" / "coin.png.cooked"), tex) && !tex.nearest);
    CHECK(db4.find_by_id(coin_id) != nullptr);

    // A deleted cooked file is noticed and remade.
    fs::remove(dir / "build" / "cooked" / "coin.png.cooked");
    AssetDatabase db5(project);
    db5.scan();
    CHECK(db5.find_by_path("coin.png")->stale());
    CHECK(db5.cook().cooked == 1);

    // Damaged cooked data is rejected, not trusted.
    std::vector<u8> junk = read_all(dir / "build" / "cooked" / "coin.png.cooked");
    junk.resize(junk.size() / 2);
    CHECK(!decode_cooked_texture(junk, tex));
    CHECK(cooked_magic(junk) == kCookedTextureMagic);
    CHECK(cooked_magic({1, 2, 3}) == 0);
}

void test_path_resolution() {
    const fs::path dir = fs::temp_directory_path() / "rewind_project_test";
    Project project;
    CHECK(load_project(dir, project));
    use_project_paths(project);

    // Cooked wins, source is the fallback, unknown stays as written.
    CHECK(resolve_asset_path("coin.png") == (project.cooked_dir() / "coin.png.cooked").string().substr(0, 0) + project.cooked_dir().generic_string() + "/coin.png.cooked" ||
          resolve_asset_path("coin.png").find("coin.png.cooked") != std::string::npos);
    CHECK(resolve_asset_path("notes.txt").find("assets") != std::string::npos);
    CHECK(resolve_asset_path("missing.png") == "missing.png");
    clear_asset_roots();
    CHECK(resolve_asset_path("coin.png") == "coin.png");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

} // namespace

int main() {
    test_create_and_load();
    test_sidecars_and_cooking();
    test_path_resolution();

    if (failures == 0) {
        std::printf("project_test: all checks passed\n");
        return 0;
    }
    std::printf("project_test: %d check(s) failed\n", failures);
    return 1;
}
