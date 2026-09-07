// Headless checks for laying roads into a network: snapping, crossings that
// become junctions, T junctions, refusals, and the lanes that come with them.

#include "../game/traffic/roads.h"

#include <cstdio>

using namespace rw;
using traffic::BuildResult;
using traffic::RoadSpec;

namespace {

int failures = 0;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

Path straight(Vec2 a, Vec2 b) { return Path::from_polyline({a, b}); }

usize live_edges(const CurveNetwork& net) {
    usize n = 0;
    for (const NetEdge& e : net.edges()) n += e.alive ? 1 : 0;
    return n;
}

usize live_nodes(const CurveNetwork& net) {
    usize n = 0;
    for (const NetNode& node : net.nodes()) n += node.alive ? 1 : 0;
    return n;
}

void test_first_road() {
    CurveNetwork net;
    RoadSpec spec;
    const BuildResult r = traffic::build_road(net, straight({0, 0}, {200, 0}), spec, 8.0f);
    CHECK(r.ok && r.edges.size() == 1 && r.junctions.size() == 2);
    CHECK(live_nodes(net) == 2 && live_edges(net) == 1);
    CHECK(net.edge(r.edges[0]).tracks.size() == 2);
    CHECK(net.node(r.junctions[0]).radius > 3.0f);
    CHECK(std::fabs(r.length - 200) < 0.5f);

    // Too short, and a loop back to where it started, are both refused.
    const BuildResult tiny = traffic::build_road(net, straight({0, 60}, {4, 60}), spec, 8.0f);
    CHECK(!tiny.ok && std::string(tiny.refusal) == "too short");
    const BuildResult loop = traffic::build_road(net, straight({0, 0}, {2, 2}), spec, 8.0f);
    CHECK(!loop.ok);
    CHECK(live_edges(net) == 1);
}

void test_crossing_makes_a_junction() {
    CurveNetwork net;
    RoadSpec spec;
    traffic::build_road(net, straight({0, 0}, {200, 0}), spec, 8.0f);
    // A road straight across the middle: both roads should be cut in two.
    const BuildResult r = traffic::build_road(net, straight({100, -100}, {100, 100}), spec, 8.0f);
    CHECK(r.ok);
    CHECK(live_edges(net) == 4);
    CHECK(live_nodes(net) == 5);
    CHECK(r.edges.size() == 2); // the new road is two pieces, one each side

    // The crossing node is where the roads met, and every arm reaches it.
    const u32 middle = r.junctions.size() > 1 ? r.junctions[1] : ~0u;
    CHECK(middle != ~0u && net.valid_node(middle));
    CHECK(distance(net.node(middle).pos, Vec2{100, 0}) < 1.0f);
    CHECK(net.edges_at(middle).size() == 4);
    // Cars can now turn from one road onto the other.
    CHECK(!net.connectors_at(middle).empty());

    // Two crossings in one drag give two junctions.
    CurveNetwork twice;
    traffic::build_road(twice, straight({0, 0}, {0, 300}), spec, 8.0f);
    traffic::build_road(twice, straight({-50, 100}, {50, 100}), spec, 8.0f);
    traffic::build_road(twice, straight({-50, 200}, {50, 200}), spec, 8.0f);
    const BuildResult across = traffic::build_road(twice, straight({-30, 50}, {-30, 260}), spec, 8.0f);
    CHECK(across.ok && across.edges.size() == 3);
}

void test_t_junction_and_snapping() {
    CurveNetwork net;
    RoadSpec spec;
    traffic::build_road(net, straight({0, 0}, {200, 0}), spec, 8.0f);
    // A road that ends on an existing one splits it into a T.
    const BuildResult t = traffic::build_road(net, straight({120, 90}, {120, 1}), spec, 8.0f);
    CHECK(t.ok);
    CHECK(live_edges(net) == 3 && live_nodes(net) == 4);
    const u32 join = t.junctions.back();
    CHECK(net.edges_at(join).size() == 3);

    // Starting near an existing junction reuses it instead of making another.
    const usize before = live_nodes(net);
    const BuildResult again = traffic::build_road(net, straight({122, 2}, {122, -90}), spec, 8.0f);
    CHECK(again.ok);
    CHECK(live_nodes(net) == before + 1); // only the far end is new
    CHECK(net.edges_at(join).size() == 4);
}

void test_curved_road_keeps_its_shape() {
    CurveNetwork net;
    RoadSpec spec;
    Path curve;
    curve.move_to({0, 0});
    curve.cubic_to({80, -80}, {160, 80}, {240, 0});
    const BuildResult r = traffic::build_road(net, curve, spec, 8.0f);
    CHECK(r.ok && r.edges.size() == 1);
    const PathSampler s(net.edge(r.edges[0]).path, 0.2f);
    CHECK(s.length() > 240); // a curve is longer than the straight line
    CHECK(std::fabs(s.at(s.length() * 0.5f).position.y) < 12);

    // Cut it in the middle: the two halves still follow the curve.
    const BuildResult cut = traffic::build_road(net, straight({120, -120}, {120, 120}), spec, 8.0f);
    CHECK(cut.ok && live_edges(net) == 4);
    for (const NetEdge& e : net.edges())
        if (e.alive) CHECK(e.path.length() > 1.0f);
}

void test_wider_roads_make_bigger_junctions() {
    CurveNetwork net;
    RoadSpec street;
    RoadSpec motorway{3, 3, 3.75f};
    traffic::build_road(net, straight({0, 0}, {300, 0}), street, 8.0f);
    const BuildResult big = traffic::build_road(net, straight({150, -150}, {150, 150}), motorway, 8.0f);
    CHECK(big.ok);
    const u32 middle = big.junctions[1];
    CHECK(net.node(middle).radius >= motorway.junction_radius() - 0.01f);
    CHECK(net.edge(big.edges[0]).tracks.size() == 6);
    CHECK(net.edge(big.edges[0]).width > 20);
}

/// A roundabout replaces whatever was in the middle and keeps the roads that
/// reached it as its entries.
void test_roundabout() {
    CurveNetwork net;
    RoadSpec street;
    RoadSpec ring{1, 0, 6.0f};
    traffic::build_road(net, straight({-200, 0}, {200, 0}), street, 8.0f);
    traffic::build_road(net, straight({0, -200}, {0, 200}), street, 8.0f);
    CHECK(live_edges(net) == 4);

    const BuildResult r = traffic::build_roundabout(net, {0, 0}, 15.0f, ring, 8.0f);
    CHECK(r.ok && r.edges.size() >= 8);
    // Nothing is left inside the island.
    for (const NetEdge& e : net.edges()) {
        if (!e.alive) continue;
        CHECK(distance(net.node(e.a).pos, Vec2{0, 0}) > 12.0f);
        CHECK(distance(net.node(e.b).pos, Vec2{0, 0}) > 12.0f);
    }
    // Four arms still reach it, and every ring piece is one way.
    usize entries = 0;
    for (const NetEdge& e : net.edges()) {
        if (!e.alive) continue;
        if (e.tracks.size() == 2) ++entries; // a street: one lane each way
    }
    CHECK(entries == 4);
    for (u32 edge : r.edges) CHECK(net.edge(edge).tracks.size() == 1 && net.edge(edge).tracks[0].forward);

    // A car can drive all the way round: every ring node has a way on and off.
    for (u32 edge : r.edges) {
        const NetEdge& e = net.edge(edge);
        CHECK(net.edges_at(e.a).size() >= 2 && net.edges_at(e.b).size() >= 2);
        CHECK(std::fabs(distance(net.node(e.a).pos, Vec2{0, 0}) - 15.0f) < 1.5f);
    }
    CHECK(!net.connectors().empty());
}

/// The smooth mode's arc: it must leave along the direction given, arrive
/// exactly at the point clicked, and bend by a believable radius.
void test_arc_between() {
    float radius = 0;
    // A quarter circle: heading east from the origin, ending 100 m east and
    // 100 m south, is an arc of radius 100 about (0, 100).
    Path arc = traffic::arc_between({0, 0}, {1, 0}, {100, 100}, &radius);
    CHECK(!arc.empty());
    CHECK(std::fabs(radius - 100) < 0.5f);
    CHECK(distance(arc.start(), Vec2{0, 0}) < 0.01f);
    CHECK(distance(arc.end(), Vec2{100, 100}) < 0.5f);
    CHECK(dot(arc.segments.front().direction(0), Vec2{1, 0}) > 0.999f);
    CHECK(dot(arc.segments.back().direction(1), Vec2{0, 1}) > 0.99f); // ends heading south
    CHECK(std::fabs(arc.length() - 3.14159265f * 100 * 0.5f) < 1.0f);

    // The mirror image bends the other way and is just as long.
    float other = 0;
    Path up = traffic::arc_between({0, 0}, {1, 0}, {100, -100}, &other);
    CHECK(std::fabs(other - 100) < 0.5f);
    CHECK(dot(up.segments.back().direction(1), Vec2{0, -1}) > 0.99f);

    // Straight ahead is a straight line, not an arc of enormous radius.
    Path line = traffic::arc_between({0, 0}, {1, 0}, {50, 0}, &radius);
    CHECK(line.segments.size() == 1 && radius == 0);
    CHECK(std::fabs(line.length() - 50) < 0.01f);

    // A tight bend reports a small radius, so the builder can warn about it.
    traffic::arc_between({0, 0}, {1, 0}, {10, 10}, &radius);
    CHECK(radius > 9 && radius < 11);

    // Turning back on itself is still a proper arc through the point.
    Path back = traffic::arc_between({0, 0}, {1, 0}, {0, 60}, &radius);
    CHECK(!back.empty() && distance(back.end(), Vec2{0, 60}) < 0.5f);
    CHECK(std::fabs(radius - 30) < 0.5f);

    // Degenerate input gives nothing rather than a broken path.
    CHECK(traffic::arc_between({0, 0}, {1, 0}, {0, 0}).empty());
    CHECK(traffic::arc_between({0, 0}, {0, 0}, {50, 0}).empty());
}

} // namespace

int main() {
    test_arc_between();
    test_roundabout();
    test_first_road();
    test_crossing_makes_a_junction();
    test_t_junction_and_snapping();
    test_curved_road_keeps_its_shape();
    test_wider_roads_make_bigger_junctions();
    if (failures == 0) std::printf("roads_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
