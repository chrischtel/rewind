// Headless checks for the surface of a network: one paved outline, islands
// where the lanes enclose room for one, keep-clear where they do not.

#include <rewind/render/mesh.h>
#include <rewind/sim/network.h>
#include <rewind/sim/network_surface.h>

#include <cmath>
#include <cstdio>

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

bool near(float a, float b, float eps = 1e-2f) { return std::fabs(a - b) <= eps; }

constexpr float kPi = 3.14159265f;

/// Arms of `length` meeting at the origin at the given angles (degrees), one
/// track each way, 3.5 wide, tracks stopping `radius` short of the centre.
CurveNetwork make_star(const std::vector<float>& degrees, float length = 80, float radius = 9) {
    CurveNetwork net;
    const u32 c = net.add_node({0, 0}, 0, 0, radius);
    for (float d : degrees) {
        const float a = d * kPi / 180;
        const u32 n = net.add_node({std::cos(a) * length, std::sin(a) * length});
        net.set_tracks(net.add_edge(n, c), 1, 1, 3.5f);
    }
    net.rebuild();
    return net;
}

/// A one-way ring of `arcs` pieces around the origin.
CurveNetwork make_ring(float radius, int arcs = 8, float width = 4) {
    CurveNetwork net;
    std::vector<u32> nodes;
    for (int i = 0; i < arcs; ++i) {
        const float a = 2 * kPi * static_cast<float>(i) / static_cast<float>(arcs);
        nodes.push_back(net.add_node({std::cos(a) * radius, std::sin(a) * radius}));
    }
    for (int i = 0; i < arcs; ++i) {
        Path arc;
        arc.move_to(net.node(nodes[static_cast<usize>(i)]).pos);
        arc.arc_to({0, 0}, 2 * kPi / static_cast<float>(arcs));
        const u32 e = net.add_edge(nodes[static_cast<usize>(i)], nodes[static_cast<usize>((i + 1) % arcs)], &arc);
        net.set_tracks(e, 1, 0, width);
    }
    net.rebuild();
    return net;
}

/// Everything a surface promises, whatever the network.
void check_partition(const NetworkSurface& s) {
    // The paving covers every lane, islands are not paved, and keep-clear is
    // paving that no lane sweeps. Boundaries are snapped to a grid of about a
    // millimetre, so "exactly" means to within a hairline along the outline:
    // a few hundredths of a square unit over a whole junction.
    CHECK(polygon_difference(s.lanes, s.paved).area() < 0.25f);
    CHECK(polygon_intersection(s.islands, s.paved).area() < 0.25f);
    CHECK(polygon_difference(s.keep_clear, s.paved).area() < 0.25f);
    CHECK(polygon_intersection(s.keep_clear, s.lanes).area() < 0.3f);
    for (const Polygon& p : s.paved.polygons) CHECK(polygon_area(p.outer) > 0);
}

void test_crossroads() {
    const CurveNetwork net = make_star({0, 90, 180, 270});
    const NetworkSurface s = network_surface(net);
    check_partition(s);
    CHECK(s.paved.size() == 1);
    CHECK(s.paved.polygons[0].holes.empty());
    CHECK(s.islands.empty());
    // On the road, in the middle of the junction, and well off it.
    CHECK(s.paved.contains({40, 0}) && s.paved.contains({0, -40}) && s.paved.contains({0, 0}));
    CHECK(s.paved.contains({40, 3.4f}) && !s.paved.contains({40, 3.7f}));
    CHECK(!s.paved.contains({30, 30}));
    // Closing rounds the inward corners, so the paving is a little more than the lanes.
    CHECK(s.paved.area() >= s.lanes.area());
    // Four arms of 80 by 7, less what they share in the middle: right order of size.
    CHECK(s.paved.area() > 4 * 70 * 7 && s.paved.area() < 4 * 80 * 7 + 300);
    // A square end where a road just stops.
    CHECK(s.paved.contains({79.5f, 0}) && !s.paved.contains({80.5f, 0}));

    // It triangulates to the same area.
    Mesh mesh;
    build_polygon_set(s.paved, 1, 1, 1, 1, mesh);
    float area = 0;
    for (usize i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const Vec3 a = mesh.vertices[mesh.indices[i]].pos, b = mesh.vertices[mesh.indices[i + 1]].pos,
                   c = mesh.vertices[mesh.indices[i + 2]].pos;
        area += std::fabs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) * 0.5f;
    }
    CHECK(near(area, s.paved.area(), s.paved.area() * 0.002f));
}

void test_odd_junctions() {
    // Nothing about the method cares how many arms there are or at what angles.
    for (const std::vector<float>& arms : std::vector<std::vector<float>>{
             {0, 180, 250},          // a road joining at an angle
             {0, 35, 180},           // a sharp fork
             {0, 72, 144, 216, 288}, // five ways
             {10, 95, 160, 200, 275, 330},
         }) {
        const CurveNetwork net = make_star(arms, 80, 12);
        const NetworkSurface s = network_surface(net);
        check_partition(s);
        CHECK(s.paved.size() == 1);
        CHECK(s.paved.contains({0, 0}));
        for (float d : arms) {
            const float a = d * kPi / 180;
            CHECK(s.paved.contains({std::cos(a) * 50, std::sin(a) * 50}));
        }
        // No spikes: opening the outline by a small radius takes almost nothing away.
        const float trimmed = polygon_open(s.paved, 0.5f).area();
        CHECK(trimmed > s.paved.area() * 0.995f);
    }
}

void test_roundabout() {
    const CurveNetwork net = make_ring(20);
    const NetworkSurface s = network_surface(net);
    check_partition(s);
    CHECK(s.paved.size() == 1);
    CHECK(s.paved.polygons[0].holes.size() == 1);
    // The middle is an island by itself: nobody had to say so.
    CHECK(s.islands.size() == 1);
    CHECK(near(s.islands.area(), kPi * 18 * 18, 25));
    CHECK(s.islands.contains({0, 0}) && !s.paved.contains({0, 0}));
    CHECK(s.paved.contains({20, 0}) && !s.paved.contains({23, 0}));
    CHECK(near(s.paved.area(), kPi * (22 * 22 - 18 * 18), 25));
    CHECK(s.keep_clear.empty());

    // A ring too small to have a middle worth keeping is paved right across,
    // and the middle is offered as keep-clear.
    const CurveNetwork tight = make_ring(3.2f, 6, 4);
    const NetworkSurface t = network_surface(tight);
    check_partition(t);
    CHECK(t.islands.empty());
    CHECK(t.paved.polygons.size() == 1 && t.paved.polygons[0].holes.empty());
    CHECK(t.paved.contains({0, 0}));
    CHECK(t.keep_clear.contains({0, 0}));

    // Raise the bar and the big ring's middle is no island either.
    SurfaceOptions strict;
    strict.min_island_area = 5000;
    const NetworkSurface none = network_surface(net, strict);
    CHECK(none.islands.empty() && none.paved.contains({0, 0}) && none.keep_clear.contains({0, 0}));
}

void test_options_and_regions() {
    const CurveNetwork net = make_star({0, 90, 180, 270});
    // Only the tracks running towards the centre, and so no connectors at all
    // (a connector needs both of its ends).
    SurfaceOptions inward;
    inward.include = [](const CurveNetwork& n, TrackRef t) { return n.edge(t.edge).tracks[t.track].forward; };
    const NetworkSurface half = network_surface(net, inward);
    CHECK(half.paved.size() == 4);
    CHECK(!half.paved.contains({0, 0}));
    CHECK(near(half.paved.area(), 4 * 71 * 3.5f, 40));

    // A verge widens everything.
    SurfaceOptions wide;
    wide.widen = 1.0f;
    CHECK(network_surface(net, wide).paved.contains({40, 3.9f}));

    // A region far away sees nothing; one over the east arm sees that arm.
    const Rect far{1000, 1000, 50, 50};
    CHECK(network_surface(net, {}, &far).empty());
    const Rect east{60, -5, 15, 10};
    const NetworkSurface part = network_surface(net, {}, &east);
    CHECK(part.paved.contains({65, 0}));
    CHECK(!part.paved.contains({-40, 0}));

    // One junction on its own: the stubs are cut square at the reach.
    const NetworkSurface node = node_surface(net, 0, 12);
    check_partition(node);
    CHECK(node.paved.size() == 1);
    CHECK(node.paved.contains({0, 0}) && node.paved.contains({18, 0}));
    CHECK(!node.paved.contains({24, 0}));
    CHECK(node_surface(net, 99).empty());
    CHECK(node_surface(net, 1, 12).paved.size() == 1); // a dead end: just its stub
}

void test_edges() {
    const CurveNetwork net = make_star({0, 180});
    // Edge 0 runs from (80, 0) to the centre; its forward track heads west,
    // offset to its own right, which is -y when heading west in y-down space.
    const TrackRef t{0, 1};
    const std::vector<Vec2> centre = track_edge(net, t, 0);
    const std::vector<Vec2> left = track_edge(net, t, -1), right = track_edge(net, t, 1);
    CHECK(centre.size() >= 2 && left.size() == centre.size() && right.size() == centre.size());
    CHECK(near(distance(left.front(), right.front()), 3.5f));
    CHECK(near(distance(left.front(), centre.front()), 1.75f));
    // The left edge of a track that keeps right is the centre line of the road.
    CHECK(near(left.front().y, 0, 0.02f));
    CHECK(near(distance(track_edge(net, t, 1, 0.25f).front(), centre.front()), 1.5f));
    CHECK(track_edge(net, TrackRef{99, 0}, 1).empty());

    const u32 c = net.find_connector(t, TrackRef{1, 0});
    CHECK(c != ~0u);
    const std::vector<Vec2> ce = connector_edge(net, c, 1);
    CHECK(ce.size() >= 2);
    // It starts where the track's edge ends.
    CHECK(near(distance(ce.front(), right.back()), 0, 0.05f));
    CHECK(connector_edge(net, 9999, 1).empty());
}

void test_taper() {
    // One road, two lanes one way; the second exists only for the last
    // hundred and opens over forty. Heading +x, a track's right is +y.
    CurveNetwork net;
    const u32 a = net.add_node({0, 0}), b = net.add_node({200, 0});
    const u32 e = net.add_edge(a, b);
    net.set_tracks(e, 2, 0, 3.5f); // the lanes cover y -3.5..0 and 0..3.5
    net.edge(e).tracks[1].begin = 100;
    net.edge(e).tracks[1].taper = 40;
    net.rebuild();
    const NetworkSurface s = network_surface(net);
    check_partition(s);
    CHECK(s.paved.size() == 1 && s.islands.empty());
    // The whole lane is there all the way; the pocket only where it runs.
    CHECK(s.paved.contains({50, -1.75f}) && s.paved.contains({150, -1.75f}));
    CHECK(!s.paved.contains({50, 1.75f}));
    CHECK(s.paved.contains({170, 3.2f}) && !s.paved.contains({170, 3.8f}));
    // It opens from the lane beside it outwards: half way along the taper the
    // paving reaches half the lane's width, and nothing is left hanging off
    // the far side.
    CHECK(s.paved.contains({120, 1.2f}) && !s.paved.contains({120, 2.4f}));
    CHECK(!s.paved.contains({103, 1.0f}));
    CHECK(s.paved.contains({139, 3.0f}));
    // No step where it starts: the outline there is a smooth run, so opening
    // it by a small radius takes next to nothing away.
    CHECK(polygon_open(s.paved, 0.3f).area() > s.paved.area() - 0.6f);

    // A line along its outer edge follows the taper; the one along the lane
    // beside it stays straight.
    const TrackRef pocket{e, 1};
    const std::vector<Vec2> outer = track_edge(net, pocket, 1), inner = track_edge(net, pocket, -1);
    CHECK(outer.size() > 8);
    CHECK(near(outer.front().y, 0, 0.05f) && near(outer.back().y, 3.5f, 0.05f));
    for (const Vec2& p : inner) CHECK(near(p.y, 0, 0.05f));
    for (usize i = 1; i < outer.size(); ++i) CHECK(outer[i].y >= outer[i - 1].y - 1e-3f); // only ever outwards

    // A lane dropped in the middle of a road closes the same way at its end.
    CurveNetwork drop;
    const u32 p = drop.add_node({0, 0}), q = drop.add_node({200, 0});
    const u32 d = drop.add_edge(p, q);
    drop.set_tracks(d, 2, 0, 3.5f);
    drop.edge(d).tracks[1].end = 100;
    drop.edge(d).tracks[1].taper = 40;
    drop.rebuild();
    const NetworkSurface ds = network_surface(drop);
    CHECK(ds.paved.contains({30, 3.0f}) && !ds.paved.contains({130, 1.0f}));
    CHECK(ds.paved.contains({80, 1.2f}) && !ds.paved.contains({80, 2.4f}));
    CHECK(!ds.paved.contains({97, 1.0f}));
}

void test_determinism() {
    const CurveNetwork net = make_star({10, 95, 160, 200, 275, 330}, 80, 12);
    const NetworkSurface a = network_surface(net);
    const NetworkSurface b = network_surface(net);
    CHECK(a.paved.point_count() == b.paved.point_count());
    bool same = a.paved.size() == b.paved.size();
    for (usize i = 0; same && i < a.paved.size(); ++i) {
        const Ring& p = a.paved.polygons[i].outer;
        const Ring& q = b.paved.polygons[i].outer;
        same = p.size() == q.size();
        for (usize k = 0; same && k < p.size(); ++k) same = p[k].x == q[k].x && p[k].y == q[k].y;
    }
    CHECK(same);
}

} // namespace

int main() {
    test_crossroads();
    test_odd_junctions();
    test_roundabout();
    test_options_and_regions();
    test_edges();
    test_taper();
    test_determinism();
    if (failures == 0) std::printf("surface_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
