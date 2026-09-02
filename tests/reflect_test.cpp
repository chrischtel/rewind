// Headless checks for reflection: save/load round trips, transient fields,
// nested structs and vectors, bulk arrays, enums, the text dump, and the
// default Game::save/load built on reflect().

#include <rewind/sim/game.h>
#include <rewind/sim/reflect.h>
#include <rewind/sim/rng.h>
#include <rewind/sim/serialize.h>

#include <cstdio>
#include <string>
#include <vector>

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

enum class Kind : u8 { Rock, Paper, Scissors };

struct Item {
    std::string name;
    i32 count = 0;
    Kind kind = Kind::Rock;

    void reflect(Visitor& v) {
        v.field("name", name);
        v.field("count", count);
        v.field("kind", kind);
    }
};

struct World {
    float x = 0;
    double precise = 0;
    bool alive = true;
    u64 big = 0;
    i16 small = 0;
    std::vector<float> samples;
    std::vector<Item> items;
    std::vector<bool> flags;
    Rng rng{5};
    float debug_only = 1.0f;
    std::string title;

    void reflect(Visitor& v) {
        v.field("x", x);
        v.field("precise", precise);
        v.field("alive", alive);
        v.field("big", big);
        v.field("small", small);
        v.field("samples", samples);
        v.field("items", items);
        v.field("flags", flags);
        v.field("rng", rng);
        v.field("debug_only", debug_only, {.transient = true});
        v.field("title", title);
    }
};

World sample_world() {
    World w;
    w.x = 1.5f;
    w.precise = 3.25;
    w.alive = false;
    w.big = 0x123456789abcdefull;
    w.small = -7;
    w.samples = {0.5f, 1.5f, 2.5f, 3.5f};
    w.items = {Item{"sword", 1, Kind::Paper}, Item{"shield", 2, Kind::Scissors}};
    w.flags = {true, false, true};
    w.rng.next_u32();
    w.debug_only = 42.0f;
    w.title = "round trip";
    return w;
}

void test_round_trip() {
    World a = sample_world();
    Writer out;
    save_reflected(a, out);

    World b; // defaults everywhere
    b.debug_only = -1.0f;
    Reader in(out.bytes());
    load_reflected(b, in);
    CHECK(in.ok());
    CHECK(in.remaining() == 0);

    CHECK(b.x == 1.5f);
    CHECK(b.precise == 3.25);
    CHECK(b.alive == false);
    CHECK(b.big == 0x123456789abcdefull);
    CHECK(b.small == -7);
    CHECK(b.samples == a.samples);
    CHECK(b.items.size() == 2);
    CHECK(b.items[0].name == "sword" && b.items[0].count == 1 && b.items[0].kind == Kind::Paper);
    CHECK(b.items[1].name == "shield" && b.items[1].kind == Kind::Scissors);
    CHECK(b.flags == a.flags);
    CHECK(b.rng.state == a.rng.state && b.rng.inc == a.rng.inc);
    CHECK(b.title == "round trip");

    // Transient fields are not written and not read.
    CHECK(b.debug_only == -1.0f);
}

void test_transient_does_not_change_bytes() {
    World a = sample_world();
    Writer out1;
    save_reflected(a, out1);
    a.debug_only = 99.0f;
    Writer out2;
    save_reflected(a, out2);
    CHECK(out1.bytes() == out2.bytes());
    CHECK(hash_bytes(out1.bytes()) == hash_bytes(out2.bytes()));
}

void test_load_resizes_vectors() {
    World a = sample_world();
    Writer out;
    save_reflected(a, out);

    World b;
    b.samples = std::vector<float>(100, 9.0f); // larger than saved
    b.items = std::vector<Item>(10);
    Reader in(out.bytes());
    load_reflected(b, in);
    CHECK(b.samples.size() == 4);
    CHECK(b.items.size() == 2);
}

void test_truncated_stream_is_safe() {
    World a = sample_world();
    Writer out;
    save_reflected(a, out);
    std::vector<u8> bytes = out.bytes();
    bytes.resize(bytes.size() / 2);

    World b;
    Reader in(bytes);
    load_reflected(b, in);
    CHECK(!in.ok());
    // Corrupt counts must not turn into giant allocations.
    std::vector<u8> evil(8, 0xff);
    World c;
    Reader in2(evil);
    load_reflected(c, in2);
    CHECK(c.samples.empty());
    CHECK(c.items.empty());
}

void test_dump() {
    World a = sample_world();
    DumpVisitor dump(2);
    a.reflect(dump);
    const std::string& t = dump.text();
    CHECK(t.find("x = 1.5\n") != std::string::npos);
    CHECK(t.find("alive = false") != std::string::npos);
    CHECK(t.find("items [2]") != std::string::npos);
    CHECK(t.find("name = \"sword\"") != std::string::npos);
    CHECK(t.find("debug_only = 42  (transient)") != std::string::npos);
    CHECK(t.find("samples [4]") != std::string::npos);
    CHECK(t.find("showing first 2") != std::string::npos);
}

/// A game that only implements reflect(): save/load must come for free.
struct ReflectedGame final : Game {
    World world;
    void tick(const TickContext&) override { world.x += 1; }
    void reflect(Visitor& v) override { v.field("world", world); }
    void render(Renderer2D&, float) override {}
};

void test_default_game_save_load() {
    ReflectedGame g;
    g.world = sample_world();
    Writer out;
    g.save(out);
    CHECK(!out.bytes().empty());

    ReflectedGame h;
    Reader in(out.bytes());
    h.load(in);
    CHECK(in.ok() && in.remaining() == 0);
    CHECK(h.world.title == "round trip");
    CHECK(h.world.items.size() == 2);
    CHECK(h.world.samples == g.world.samples);
}

} // namespace

int main() {
    test_round_trip();
    test_transient_does_not_change_bytes();
    test_load_resizes_vectors();
    test_truncated_stream_is_safe();
    test_dump();
    test_default_game_save_load();

    if (failures == 0) {
        std::printf("reflect_test: all checks passed\n");
        return 0;
    }
    std::printf("reflect_test: %d check(s) failed\n", failures);
    return 1;
}
