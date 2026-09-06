// Headless checks for SpatialGrid against brute force.

#include <rewind/core/spatial.h>
#include <rewind/sim/rng.h>

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace rw;

namespace {

int failures = 0;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                         \
        }                                                                                      \
    } while (0)

template <class G> std::vector<u32> ids_in(const G& grid, const Rect& area) {
    std::vector<u32> out;
    grid.query(area, [&](u32, const u32& id, const Rect&) { out.push_back(id); });
    std::sort(out.begin(), out.end());
    return out;
}

void test_basics() {
    SpatialGrid<u32> grid(32);
    const u32 a = grid.insert(1, {0, 0, 10, 10});
    const u32 b = grid.insert(2, {100, 100, 10, 10});
    const u32 c = grid.insert(3, {-20, -20, 300, 300}); // spans many cells
    CHECK(grid.size() == 3);
    CHECK(ids_in(grid, {0, 0, 5, 5}) == std::vector<u32>({1, 3}));
    CHECK(ids_in(grid, {95, 95, 20, 20}) == std::vector<u32>({2, 3}));
    CHECK(ids_in(grid, {50, 50, 1, 1}) == std::vector<u32>({3}));
    CHECK(ids_in(grid, {1000, 1000, 1, 1}).empty());
    CHECK(ids_in(grid, {-100, -100, 1000, 1000}) == std::vector<u32>({1, 2, 3})); // each once despite many cells

    grid.update(a, {200, 200, 10, 10});
    CHECK(ids_in(grid, {0, 0, 5, 5}) == std::vector<u32>({3}));
    CHECK(ids_in(grid, {205, 205, 1, 1}) == std::vector<u32>({1, 3}));
    grid.remove(c);
    CHECK(grid.size() == 2 && !grid.valid(c));
    CHECK(ids_in(grid, {205, 205, 1, 1}) == std::vector<u32>({1}));
    const u32 d = grid.insert(4, {0, 0, 1, 1});
    CHECK(d == c); // handle reused
    CHECK(grid.item(d) == 4);
    grid.remove(b);
    grid.remove(a);
    grid.remove(d);
    CHECK(grid.empty());
    grid.remove(a); // double remove is harmless
    CHECK(grid.empty());
}

void test_queries() {
    SpatialGrid<u32> grid(16);
    grid.insert(1, {0, 0, 10, 10});
    grid.insert(2, {50, 0, 10, 10});
    grid.insert(3, {0, 50, 10, 10});
    grid.insert(4, {200, 200, 10, 10});

    std::vector<u32> circle;
    grid.query_circle({30, 5}, 21, [&](u32, const u32& id, const Rect&) { circle.push_back(id); });
    std::sort(circle.begin(), circle.end());
    CHECK(circle == std::vector<u32>({1, 2}));
    circle.clear();
    grid.query_circle({30, 5}, 19, [&](u32, const u32& id, const Rect&) { circle.push_back(id); });
    CHECK(circle.empty());

    std::vector<u32> seg;
    grid.query_segment({-5, 5}, {70, 5}, [&](u32, const u32& id, const Rect&) { seg.push_back(id); });
    std::sort(seg.begin(), seg.end());
    CHECK(seg == std::vector<u32>({1, 2}));
    seg.clear();
    grid.query_segment({-5, 20}, {70, 20}, [&](u32, const u32& id, const Rect&) { seg.push_back(id); });
    CHECK(seg.empty());
    seg.clear();
    grid.query_segment({-5, -5}, {60, 60}, [&](u32, const u32& id, const Rect&) { seg.push_back(id); }); // diagonal hits only 1
    CHECK(seg == std::vector<u32>({1}));

    auto n = grid.nearest({5, 5}, 100);
    CHECK(n && grid.item(*n) == 1);
    n = grid.nearest({30, 5}, 100);
    CHECK(n && (grid.item(*n) == 1 || grid.item(*n) == 2)); // equidistant
    n = grid.nearest({150, 150}, 100);
    CHECK(n && grid.item(*n) == 4);
    n = grid.nearest({150, 150}, 10);
    CHECK(!n);
    n = grid.nearest({-1000, -1000}, 5000);
    CHECK(n && grid.item(*n) == 1);

    const Rect e = grid.extent();
    CHECK(e.x == 0 && e.y == 0 && e.right() == 210 && e.bottom() == 210);
    int visited = 0;
    grid.each([&](u32, const u32&, const Rect&) { ++visited; });
    CHECK(visited == 4);
}

void test_random_against_brute_force() {
    Rng rng(7);
    SpatialGrid<u32> grid(25);
    struct Item {
        u32 handle;
        Rect r;
    };
    std::vector<Item> items;
    auto random_rect = [&]() {
        const float x = rng.range_f(-500, 500), y = rng.range_f(-500, 500);
        return Rect{x, y, rng.range_f(1, 60), rng.range_f(1, 60)};
    };
    for (u32 i = 0; i < 400; ++i) {
        const Rect r = random_rect();
        items.push_back({grid.insert(i, r), r});
    }
    // Move some, remove some.
    for (int k = 0; k < 150; ++k) {
        const usize i = static_cast<usize>(rng.range(0, 399));
        items[i].r = random_rect();
        grid.update(items[i].handle, items[i].r);
    }
    std::vector<bool> alive(400, true);
    for (int k = 0; k < 50; ++k) {
        const usize i = static_cast<usize>(rng.range(0, 399));
        if (!alive[i]) continue;
        grid.remove(items[i].handle);
        alive[i] = false;
    }

    for (int q = 0; q < 200; ++q) {
        const Rect area = random_rect();
        std::vector<u32> expected;
        for (u32 i = 0; i < 400; ++i)
            if (alive[i] && overlaps(items[i].r, area)) expected.push_back(i);
        CHECK(ids_in(grid, area) == expected);

        const Vec2 p{rng.range_f(-600, 600), rng.range_f(-600, 600)};
        float best = 1e30f;
        for (u32 i = 0; i < 400; ++i)
            if (alive[i]) best = std::min(best, rect_distance(items[i].r, p));
        auto n = grid.nearest(p, 2000);
        CHECK(n.has_value());
        if (n) CHECK(std::fabs(rect_distance(grid.bounds(*n), p) - best) < 1e-3f);
    }
}

} // namespace

int main() {
    test_basics();
    test_queries();
    test_random_against_brute_force();
    if (failures == 0) std::printf("spatial_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
