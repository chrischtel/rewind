// Headless checks for the graph: building, A*/Dijkstra, grids, floods, reflection.

#include <rewind/sim/graph.h>
#include <rewind/sim/serialize.h>

#include <cmath>
#include <cstdio>
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

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

void test_basic_paths() {
    Graph g;
    const u32 a = g.add_node({0, 0}), b = g.add_node({10, 0}), c = g.add_node({10, 10}), d = g.add_node({0, 10});
    const u32 e = g.add_node({5, 5});
    g.connect(a, b);
    g.connect(b, c);
    g.connect(c, d);
    g.connect(d, a);
    g.add_edge(a, e, 100); // expensive shortcut nobody should take
    g.add_edge(e, c, 100);

    GraphPath p = g.shortest_path(a, c);
    CHECK(p.found && near(p.cost, 20));
    CHECK(p.nodes.size() == 3 && p.nodes.front() == a && p.nodes.back() == c);
    CHECK(p.nodes[1] == b); // tie between a-b-c and a-d-c: lower id wins deterministically

    // Dijkstra (no heuristic) agrees.
    GraphPath q = g.shortest_path(a, c, 0);
    CHECK(q.found && near(q.cost, 20) && q.nodes == p.nodes);

    // Directed edge only works one way.
    Graph h;
    const u32 x = h.add_node({0, 0}), y = h.add_node({1, 0});
    h.add_edge(x, y, 1, true);
    CHECK(h.shortest_path(x, y).found);
    CHECK(!h.shortest_path(y, x).found);

    // Disabled edge and removed node block routes.
    const u32 ab = g.find_edge(a, b);
    CHECK(ab != Graph::kNone);
    g.remove_edge(ab);
    GraphPath r = g.shortest_path(a, c);
    CHECK(r.found && r.nodes[1] == d);
    g.remove_node(d);
    GraphPath s = g.shortest_path(a, c);
    CHECK(s.found && near(s.cost, 200) && s.nodes[1] == e);
    CHECK(!g.valid(d));
    g.remove_node(e);
    CHECK(!g.shortest_path(a, c).found);
    CHECK(!g.shortest_path(a, 999).found);

    // Passable filter.
    Graph line;
    const u32 n0 = line.add_node({0, 0}), n1 = line.add_node({1, 0}), n2 = line.add_node({2, 0});
    line.connect(n0, n1);
    line.connect(n1, n2);
    CHECK(line.shortest_path(n0, n2).found);
    CHECK(!line.shortest_path(n0, n2, 1, [&](u32 n) { return n != n1; }).found);
    CHECK(line.nearest_node({1.2f, 0}) == n1);
    CHECK(line.nearest_node({50, 0}, 5) == Graph::kNone);
}

void test_grid() {
    // 5x5 with a wall down column 2 except the bottom row.
    auto passable = [](u32 x, u32 y) { return !(x == 2 && y < 4); };
    Graph g = Graph::grid(5, 5, 10, false, passable);
    CHECK(g.node_count() == 25);
    CHECK(!g.valid(2));
    GraphPath p = g.shortest_path(0, 4); // top-left to top-right: around the wall via the bottom
    CHECK(p.found);
    CHECK(near(p.cost, 120)); // down 4, right 4, up 4 = 12 steps
    for (u32 n : p.nodes) CHECK(g.valid(n));
    const std::vector<Vec2> pts = g.positions(p);
    CHECK(pts.size() == p.nodes.size() && pts.front().x == 0 && pts.back().x == 40);

    // Diagonals shorten open routes but never cut corners.
    Graph d = Graph::grid(3, 3, 1, true);
    GraphPath diag = d.shortest_path(0, 8);
    CHECK(diag.found && near(diag.cost, 2 * 1.41421356f, 1e-3f) && diag.nodes.size() == 3);
    Graph corner = Graph::grid(2, 2, 1, true, [](u32 x, u32 y) { return !(x == 1 && y == 0) && !(x == 0 && y == 1); });
    CHECK(!corner.shortest_path(0, 3).found);
}

void test_flood() {
    Graph g = Graph::grid(4, 4, 1);
    const std::vector<float> dist = g.distances_from(0);
    CHECK(near(dist[15], 6) && near(dist[5], 2) && near(dist[0], 0));
    const std::vector<u32> within = g.reachable(0, 2);
    CHECK(within.size() == 6); // 0, 1, 4, 2, 5, 8
    CHECK(within[0] == 0 && within[1] == 1 && within[2] == 4);
    const std::vector<float> capped = g.distances_from(0, 1);
    CHECK(capped[5] >= 1e30f && near(capped[1], 1));
}

void test_reflection() {
    Graph g;
    g.add_node({0, 0});
    g.add_node({3, 4});
    g.connect(0, 1);
    Writer w;
    {
        SaveVisitor save(w);
        g.reflect(save);
    }
    Graph loaded;
    {
        Reader r(w.bytes());
        LoadVisitor load(r);
        loaded.reflect(load);
    }
    CHECK(loaded.node_count() == 2 && loaded.edge_count() == 2);
    GraphPath p = loaded.shortest_path(0, 1);
    CHECK(p.found && near(p.cost, 5)); // adjacency rebuilt after load
}

} // namespace

void test_bidirectional_and_penalties() {
    // A grid with varied costs: both searches must agree everywhere.
    Graph g = Graph::grid(12, 9, 1, true);
    u64 seed = 12345;
    auto rnd = [&]() {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>((seed >> 33) % 100) / 25.0f + 0.5f;
    };
    for (u32 e = 0; e < g.edge_count(); ++e) g.set_edge_cost(e, rnd());
    int compared = 0;
    for (u32 from = 0; from < 108; from += 7)
        for (u32 to = 5; to < 108; to += 11) {
            const GraphPath a = g.shortest_path(from, to, 0);
            const GraphPath b = g.bidirectional_path(from, to);
            CHECK(a.found == b.found);
            if (a.found) CHECK(near(a.cost, b.cost, 1e-3f));
            CHECK(b.nodes.front() == from && b.nodes.back() == to);
            // The returned node chain really costs what it claims.
            float sum = 0;
            for (usize i = 0; i + 1 < b.nodes.size(); ++i) sum += g.edge_cost(g.find_edge(b.nodes[i], b.nodes[i + 1]));
            CHECK(near(sum, b.cost, 1e-3f));
            ++compared;
        }
    CHECK(compared > 100);
    CHECK(g.bidirectional_path(3, 3).found && g.bidirectional_path(3, 3).nodes.size() == 1);
    CHECK(!g.bidirectional_path(3, 999).found);

    // Penalties steer routes without touching the authored cost, and bump the version.
    Graph line = Graph::grid(3, 2, 1); // two rows: 0-1-2 over 3-4-5
    const u64 before = line.version();
    const u32 e01 = line.find_edge(0, 1);
    line.set_edge_penalty(e01, 10);
    CHECK(line.version() > before);
    CHECK(near(line.edge(e01).cost, 1) && near(line.edge_cost(e01), 11));
    GraphPath around = line.shortest_path(0, 2, 0);
    CHECK(around.found && around.nodes.size() == 5); // down, across, up
    line.clear_penalties();
    CHECK(line.shortest_path(0, 2, 0).nodes.size() == 3);
    line.set_edge_penalty(e01, 10);
    CHECK(line.bidirectional_path(0, 2).nodes.size() == 5);

    // Flow field: every node knows its next edge towards the goal.
    Graph::FlowField f = g.flow_to(107);
    CHECK(f.goal == 107 && f.reachable(0) && !f.reachable(999));
    u32 at = 0;
    int steps = 0;
    while (at != 107 && steps++ < 200) {
        const u32 e = f.next_edge[at];
        CHECK(e != Graph::kNone);
        if (e == Graph::kNone) break;
        at = g.edge(e).to;
    }
    CHECK(at == 107);
    CHECK(near(f.distance[0], g.shortest_path(0, 107, 0).cost, 1e-3f));
    CHECK(f.next_edge[107] == Graph::kNone && near(f.distance[107], 0));
    Graph::FlowField capped = g.flow_to(107, 1.0f);
    CHECK(!capped.reachable(0));

    // The cache hands back stored routes until the version moves.
    RouteCache cache(2);
    CHECK(cache.find(0, 5, g.version()) == nullptr && cache.misses() == 1);
    const GraphPath& stored = cache.get(0, 5, g.version(), [&] { return g.shortest_path(0, 5); });
    CHECK(stored.found && cache.size() == 1);
    CHECK(cache.find(0, 5, g.version()) != nullptr && cache.hits() == 1);
    CHECK(cache.find(0, 5, g.version() + 1) == nullptr); // stale
    cache.get(1, 5, g.version(), [&] { return g.shortest_path(1, 5); });
    cache.get(2, 5, g.version(), [&] { return g.shortest_path(2, 5); }); // evicts (0,5)
    CHECK(cache.size() == 2 && cache.find(0, 5, g.version()) == nullptr);
}

int main() {
    test_bidirectional_and_penalties();
    test_basic_paths();
    test_grid();
    test_flood();
    test_reflection();
    if (failures == 0) std::printf("graph_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
