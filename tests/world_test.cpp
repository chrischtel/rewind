// Headless checks for tilemap collision. No renderer needed.

#include <rewind/world/tilemap.h>

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

bool near(float a, float b, float eps = 0.0001f) { return a - b < eps && b - a < eps; }

TileMap room() {
    // 8x6 tiles of 16 px, walls all around, a block in the middle.
    return tilemap_from_text(
        {
            "########",
            "#......#",
            "#..##..#",
            "#......#",
            "#......#",
            "########",
        },
        "#", 16, 16);
}

void test_text_builder() {
    const TileMap m = tilemap_from_text({"a.b", ".c."}, "abc", 8, 8);
    CHECK(m.width == 3 && m.height == 2);
    CHECK(m.id_at(0, 0) == 1);
    CHECK(m.id_at(1, 0) == 0);
    CHECK(m.id_at(2, 0) == 2);
    CHECK(m.id_at(1, 1) == 3);
    CHECK(m.is_solid(0, 0) && !m.is_solid(1, 0));
    CHECK(m.is_solid(-1, 0)); // outside counts as wall by default
    CHECK(near(m.pixel_width(), 24) && near(m.pixel_height(), 16));

    TileMap soft = m;
    soft.solid = {0, 1, 0, 0}; // only id 1 is solid
    CHECK(soft.is_solid(0, 0) && !soft.is_solid(2, 0) && !soft.is_solid(1, 1));
    soft.solid_outside = false;
    CHECK(!soft.is_solid(-1, 0));
}

void test_tile_src() {
    TileMap m;
    m.tile_width = 16;
    m.tile_height = 16;
    m.tileset_columns = 4;
    const Rect r = m.tile_src(6);
    CHECK(near(r.x, 32) && near(r.y, 16) && near(r.w, 16) && near(r.h, 16));
    m.tileset_columns = 1;
    CHECK(near(m.tile_src(3).y, 48) && near(m.tile_src(3).x, 0));
}

void test_move_stops_at_walls() {
    const TileMap m = room();
    Rect box{20, 20, 10, 10}; // inside the top-left free cell, row 1 is open

    TileHit h = tilemap_move(m, box, 100, 0); // slam right into the far wall
    CHECK(h.right && !h.left && !h.top && !h.bottom);
    CHECK(near(box.x, 112 - 10)); // wall at tile 7 -> x = 112

    Rect low{20, 36, 10, 10}; // row 2 has the block at tiles 3 and 4
    h = tilemap_move(m, low, 100, 0);
    CHECK(h.right);
    CHECK(near(low.x, 48 - 10)); // block starts at tile 3 -> x = 48

    h = tilemap_move(m, box, -100, 0);
    CHECK(h.left);
    CHECK(near(box.x, 16));

    h = tilemap_move(m, box, 0, -100);
    CHECK(h.top);
    CHECK(near(box.y, 16));

    h = tilemap_move(m, box, 0, 100);
    CHECK(h.bottom);
    CHECK(near(box.y, 80 - 10)); // floor at tile row 5 -> y = 80
}

void test_move_free_space() {
    const TileMap m = room();
    Rect box{20, 20, 8, 8};
    const TileHit h = tilemap_move(m, box, 5, 3);
    CHECK(!h.any());
    CHECK(near(box.x, 25) && near(box.y, 23));
}

void test_diagonal_resolves_x_then_y() {
    const TileMap m = room();
    // Standing on the floor, moving diagonally down-right: x moves, y is blocked.
    Rect box{20, 70, 10, 10};
    const TileHit h = tilemap_move(m, box, 4, 20);
    CHECK(!h.right && h.bottom);
    CHECK(near(box.x, 24) && near(box.y, 70));
}

void test_overlaps_and_points() {
    const TileMap m = room();
    CHECK(m.solid_at(0, 0));
    CHECK(!m.solid_at(20, 20));
    CHECK(m.solid_at(50, 40)); // middle block
    CHECK(m.overlaps_solid(Rect{40, 30, 10, 10}));
    CHECK(!m.overlaps_solid(Rect{17, 17, 14, 14}));
    // Touching an edge exactly is not overlapping.
    CHECK(!m.overlaps_solid(Rect{16, 16, 32, 16}));
}

void test_determinism() {
    const TileMap m = room();
    Rect a{20, 20, 10, 10}, b{20, 20, 10, 10};
    for (int i = 0; i < 1000; ++i) {
        const float dx = (i % 7) * 3.1f - 9.0f;
        const float dy = (i % 5) * 2.7f - 5.0f;
        tilemap_move(m, a, dx, dy);
        tilemap_move(m, b, dx, dy);
    }
    CHECK(a.x == b.x && a.y == b.y);
    CHECK(!m.overlaps_solid(a));
}

} // namespace

int main() {
    test_text_builder();
    test_tile_src();
    test_move_stops_at_walls();
    test_move_free_space();
    test_diagonal_resolves_x_then_y();
    test_overlaps_and_points();
    test_determinism();

    if (failures == 0) {
        std::printf("world_test: all checks passed\n");
        return 0;
    }
    std::printf("world_test: %d check(s) failed\n", failures);
    return 1;
}
