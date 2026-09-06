// Headless checks for save games and localisation.

#include <rewind/core/localization.h>
#include <rewind/core/paths.h>
#include <rewind/render/renderer2d.h>
#include <rewind/sim/game.h>
#include <rewind/sim/savegame.h>
#include <rewind/sim/serialize.h>

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

/// Version 1 stored only `score`; version 2 added `lives`.
class V2Game final : public Game {
public:
    i32 score = 0;
    i32 lives = 3;
    void tick(const TickContext&) override {}
    void render(Renderer2D&, float) override {}
    void reflect(Visitor& v) override {
        v.field("score", score);
        v.field("lives", lives);
    }
};

void test_savegames() {
    const fs::path dir = fs::temp_directory_path() / "rewind_savegame_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string a = (dir / "slot_a.rwsave").string();

    V2Game game;
    game.score = 1234;
    game.lives = 1;
    std::string error;
    CHECK(write_savegame(a, game, 2, "after the boss", 600, &error));

    SaveInfo info;
    CHECK(peek_savegame(a, info, &error));
    CHECK(info.version == 2 && info.label == "after the boss" && info.tick == 600 && info.bytes == 8);
    CHECK(info.timestamp > 1700000000ull);
    CHECK(!format_timestamp(info.timestamp).empty());

    V2Game loaded;
    SaveInfo read_info;
    CHECK(read_savegame(a, loaded, 2, nullptr, &read_info, &error));
    CHECK(loaded.score == 1234 && loaded.lives == 1 && read_info.label == info.label);

    // An older save: version 1 had no `lives`. Without migration it is
    // refused; with one, the migration appends the missing field.
    {
        Writer w;
        w.pod<i32>(77);
        std::vector<u8> old_state = w.bytes();
        // Write a v1 file by hand through a tiny game that only has `score`.
        class V1Game final : public Game {
        public:
            i32 score = 77;
            void tick(const TickContext&) override {}
            void render(Renderer2D&, float) override {}
            void reflect(Visitor& v) override { v.field("score", score); }
        } v1;
        CHECK(write_savegame((dir / "old.rwsave").string(), v1, 1, "old", 10, &error));
    }
    V2Game from_old;
    CHECK(!read_savegame((dir / "old.rwsave").string(), from_old, 2, nullptr, nullptr, &error) && !error.empty());
    u32 seen_version = 0;
    const SaveMigration migrate = [&](u32 from, std::vector<u8>& state) {
        seen_version = from;
        if (from != 1) return false;
        Writer w;
        w.pod<i32>(3); // lives added in v2
        state.insert(state.end(), w.bytes().begin(), w.bytes().end());
        return true;
    };
    CHECK(read_savegame((dir / "old.rwsave").string(), from_old, 2, migrate, nullptr, &error));
    CHECK(seen_version == 1 && from_old.score == 77 && from_old.lives == 3);

    // A refusing migration and a damaged file both fail cleanly.
    CHECK(!read_savegame((dir / "old.rwsave").string(), from_old, 2, [](u32, std::vector<u8>&) { return false; }, nullptr, &error));
    {
        std::ofstream junk(dir / "junk.rwsave", std::ios::binary);
        junk << "not a save";
    }
    CHECK(!peek_savegame((dir / "junk.rwsave").string(), info, &error));
    CHECK(!read_savegame((dir / "missing.rwsave").string(), from_old, 2, nullptr, nullptr, &error));

    const std::vector<SaveInfo> list = list_savegames(dir.string());
    CHECK(list.size() == 2); // the junk file is skipped
    CHECK(list[0].timestamp >= list[1].timestamp);

    CHECK(!savegame_dir("RewindTest", "savegame_test").empty());
    fs::remove_all(dir);
}

void test_localization() {
    const fs::path dir = fs::temp_directory_path() / "rewind_loc_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "strings");
    {
        std::ofstream en(dir / "strings" / "en.json");
        en << "{ \"menu\": { \"play\": \"Play\", \"quit\": \"Quit\" }, \"hello\": \"Hello, {name}!\", \"lives\": \"{n} lives\" }";
        std::ofstream de(dir / "strings" / "de.json");
        de << "{ \"menu\": { \"play\": \"Spielen\" }, \"hello\": \"Hallo, {name}!\" }";
    }
    clear_asset_roots();
    clear_asset_packs();
    add_asset_root(dir.string(), false);

    Localization loc;
    loc.configure("strings", "en");
    CHECK(loc.text("menu.play") == "Play");
    CHECK(loc.set_language("de"));
    CHECK(loc.language() == "de");
    CHECK(loc.text("menu.play") == "Spielen");
    CHECK(loc.text("menu.quit") == "Quit");      // fallback fills the gap
    CHECK(loc.text("menu.nope") == "[menu.nope]"); // missing keys stay visible
    CHECK(loc.has("hello") && !loc.has("menu.nope"));
    CHECK(loc.format("hello", {{"name", "Chris"}}) == "Hallo, Chris!");
    CHECK(loc.format("lives", {{"n", "3"}}) == "3 lives");
    CHECK(!loc.set_language("fr")); // missing table: falls back entirely
    CHECK(loc.text("menu.play") == "Play");
    const std::vector<std::string> langs = loc.available();
    CHECK(langs.size() == 2);

    StringTable bad;
    std::string error;
    CHECK(!bad.load_json("[1, 2]", &error) && !error.empty());
    CHECK(!bad.load_json("{ nope", &error));

    clear_asset_roots();
    fs::remove_all(dir);
}

} // namespace

int main() {
    test_savegames();
    test_localization();
    if (failures == 0) std::printf("savegame_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
