// Headless checks for data tables: loading both JSON shapes, lookup,
// validation, editing, DefRef caching, the registry, files and asset refs.

#include <rewind/core/paths.h>
#include <rewind/project/asset_ref.h>
#include <rewind/project/data_table.h>
#include <rewind/sim/text_format.h>

#include <cstdio>
#include <filesystem>

using namespace rw;

namespace {

int failures = 0;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

struct Stats {
    float hp = 10;
    float armour = 0;
    void reflect(Visitor& v) {
        v.field("hp", hp);
        v.field("armour", armour);
    }
};

struct UnitDef {
    std::string id;
    float speed = 1;
    u8 tier = 0;
    Stats stats;
    std::vector<std::string> tags;
    AssetRef sprite;
    void reflect(Visitor& v) {
        v.field("id", id);
        v.field("speed", speed);
        v.field("tier", tier);
        v.field("stats", stats);
        v.field("tags", tags);
        v.field("sprite", sprite, {.asset_type = "texture"});
    }
};

struct Wave {
    std::string id;
    DefRef<UnitDef> unit;
    int count = 1;
    void reflect(Visitor& v) {
        v.field("id", id);
        v.field("unit", unit, {.table = "units"});
        v.field("count", count);
    }
};

const char* kUnits = R"({"rows": [
  {"id": "scout", "speed": 3, "tier": 1, "stats": {"hp": 4}, "tags": ["fast", "light"]},
  {"id": "tank", "speed": 0.5, "tier": 3, "stats": {"hp": 40, "armour": 5}, "sprite": {"id": "00000000deadbeef", "path": "sprites/tank.png"}}
]})";

void test_load_and_lookup() {
    DataTable<UnitDef> units{"units", "data/units.json"};
    std::string error;
    CHECK(units.load_text(kUnits, &error));
    CHECK(units.size() == 2 && units.problems().empty());
    CHECK(units.index_of("tank") == 1 && units.index_of("nope") == -1 && units.index_of("") == -1);
    const UnitDef* tank = units.find("tank");
    CHECK(tank && tank->speed == 0.5f && tank->tier == 3 && tank->stats.hp == 40 && tank->stats.armour == 5);
    CHECK(tank->sprite.path == "sprites/tank.png" && tank->sprite.id == "00000000deadbeef");
    const UnitDef* scout = units.find("scout");
    CHECK(scout && scout->tags.size() == 2 && scout->tags[1] == "light" && scout->stats.armour == 0);

    // A bare array is the same document.
    DataTable<UnitDef> bare{"bare"};
    CHECK(bare.load_text(R"([{"id": "a"}, {"id": "b", "speed": 2}])", &error));
    CHECK(bare.size() == 2 && bare.find("b")->speed == 2);

    // Bad documents say why.
    CHECK(!bare.load_text("{\"units\": []}", &error) && error.find("rows") != std::string::npos);
    CHECK(!bare.load_text("[1, 2", &error) && !error.empty());
    CHECK(bare.size() == 2); // untouched by a failed load

    // Round trip through text.
    DataTable<UnitDef> copy{"copy"};
    CHECK(copy.load_text(units.to_text(), &error));
    CHECK(copy.size() == 2 && copy.find("tank")->stats.armour == 5 && copy.find("scout")->tags[0] == "fast");
}

void test_validation_and_editing() {
    DataTable<UnitDef> units{"edit"};
    std::string error;
    CHECK(units.load_text(R"([{"id": "a"}, {"id": ""}, {"id": "a"}])", &error));
    CHECK(units.problems().size() == 2);
    CHECK(units.problems()[0].find("no id") != std::string::npos);
    CHECK(units.problems()[1].find("duplicate") != std::string::npos);
    CHECK(units.index_of("a") == 0); // the first one wins

    const u32 v0 = units.version();
    CHECK(units.add_row("b") && units.size() == 4 && units.version() == v0 + 1);
    CHECK(!units.add_row("a") && !units.add_row(""));
    units.at(3).speed = 7;
    CHECK(units.duplicate_row(3, "c") && units.size() == 5 && units.at(4).speed == 7 && units.at(4).id == "c");
    CHECK(!units.duplicate_row(3, "b"));
    CHECK(units.remove_row(1) && units.remove_row(1) && units.problems().empty() && units.size() == 3);
    CHECK(!units.remove_row(99));
    // Renaming through at() needs changed().
    units.at(0).id = "renamed";
    CHECK(units.index_of("renamed") == -1);
    units.changed();
    CHECK(units.index_of("renamed") == 0 && units.index_of("a") == -1);

    // Rows edit through the generic reflect_row too.
    JsonWriteVisitor w;
    CHECK(units.reflect_row(0, w));
    CHECK(w.finish().find("\"renamed\"") != std::string::npos);
    CHECK(!units.reflect_row(99, w));
}

void test_def_ref_and_registry() {
    DataTable<UnitDef> units{"units"};
    std::string error;
    CHECK(units.load_text(kUnits, &error));
    CHECK(DataTableBase::find("units") == &units && DataTable<UnitDef>::find_table("units") == &units);
    CHECK(DataTable<Wave>::find_table("units") == nullptr); // wrong row type

    DataTable<Wave> waves{"waves"};
    CHECK(waves.load_text(R"([{"id": "w1", "unit": {"id": "tank"}, "count": 3}, {"id": "w2", "unit": {"id": "ghost"}}])", &error));
    const Wave& w1 = waves.at(0);
    CHECK(w1.unit.get(units) == units.find("tank") && w1.unit.get("units") == units.find("tank"));
    CHECK(waves.at(1).unit.get(units) == nullptr && waves.at(1).unit.get("nowhere") == nullptr);

    // The cache refreshes when the table changes: the row moves, the pointer follows.
    const UnitDef* before = w1.unit.get(units);
    CHECK(units.remove_row(0));
    const UnitDef* after = w1.unit.get(units);
    CHECK(after == units.find("tank") && after->id == "tank" && after != before);
    CHECK(units.add_row("scout") && w1.unit.get(units)->id == "tank");

    // DefRef saves as its id and compares by id.
    Wave wave;
    wave.id = "x";
    wave.unit = DefRef<UnitDef>{"scout"};
    const std::string json = to_json(wave);
    CHECK(json.find("\"scout\"") != std::string::npos);
    Wave back;
    CHECK(from_json(json, back, &error) && back.unit == wave.unit && back.unit.id == "scout");

    {
        DataTable<UnitDef> temp{"temp"};
        CHECK(DataTableBase::find("temp") == &temp && DataTableBase::all().size() == 3);
    }
    CHECK(DataTableBase::find("temp") == nullptr && DataTableBase::all().size() == 2);
}

void test_files() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "rewind_data_table_test";
    std::filesystem::create_directories(dir / "data");
    clear_asset_roots();
    add_asset_root(dir.string(), false);
    std::string error;
    CHECK(write_text_file((dir / "data" / "units.json").string(), kUnits, &error));

    DataTable<UnitDef> units{"units", "data/units.json"};
    CHECK(units.load(&error));
    CHECK(units.size() == 2);
    DataTable<UnitDef> missing{"missing", "data/nothing.json"};
    CHECK(!missing.load(&error) && error.find("nothing.json") != std::string::npos);

    // Save writes back where it came from; a reload sees the edit.
    units.at(0).speed = 9;
    CHECK(units.save(&error));
    DataTable<UnitDef> again{"again", "data/units.json"};
    CHECK(again.load(&error) && again.find("scout")->speed == 9);
    std::filesystem::remove_all(dir);
    clear_asset_roots();
}

} // namespace

int main() {
    test_load_and_lookup();
    test_validation_and_editing();
    test_def_ref_and_registry();
    test_files();
    if (failures == 0) std::printf("data_table_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
