// Headless checks for laying roads into a network: snapping, crossings that
// become junctions, T junctions, refusals, and the lanes that come with them.

#include "../game/traffic/lanes.h"
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

/// The game's lane discipline, which a bare network does not have.
void use_lane_rules(CurveNetwork& net) {
    net.set_connect_filter(
        [](const CurveNetwork& n, u32, TrackRef from, TrackRef to) { return traffic::may_connect(n, from, to); });
}

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

/// Lane discipline: which lane may go where, and what happens where a road
/// loses a lane.
void test_lane_rules() {
    using traffic::LaneKind;
    CurveNetwork net;
    use_lane_rules(net);
    RoadSpec wide;
    wide.lanes = {{0, false, 3.25f, 0}, {0, false, 3.25f, 0}, {0, true, 3.25f, 0}, {0, true, 3.25f, 0}};
    RoadSpec narrow;
    narrow.lanes = {{0, false, 3.25f, 0}, {0, true, 3.25f, 0}};

    // A four-lane road running into a two-lane one: the road loses a lane
    // each way, and only the kerb lanes carry on.
    const BuildResult a = traffic::build_road(net, straight({0, 0}, {200, 0}), wide, 8.0f);
    const BuildResult b = traffic::build_road(net, straight({200, 0}, {400, 0}), narrow, 8.0f);
    CHECK(a.ok && b.ok);
    const u32 first = a.edges[0], second = b.edges[0];

    // Ranks count from each direction's own kerb.
    const NetEdge& e = net.edge(first);
    CHECK(traffic::rank_from_kerb(e, 3) == 0); // the outermost forward lane is the kerb lane
    CHECK(traffic::rank_from_kerb(e, 2) == 1);
    CHECK(traffic::rank_from_kerb(e, 0) == 0); // and the same the other way
    CHECK(traffic::rank_from_kerb(e, 1) == 1);

    // The kerb lane continues; the inner one has nowhere to go, so its cars
    // must move over first. That is the merge.
    CHECK(traffic::may_connect(net, TrackRef{first, 3}, TrackRef{second, 1}));
    CHECK(!traffic::may_connect(net, TrackRef{first, 2}, TrackRef{second, 1}));

    // Nothing connects to a cycle lane or out of one.
    RoadSpec with_cycle;
    with_cycle.lanes = {{0, false, 3.0f, static_cast<u32>(LaneKind::Driving)},
                        {0, true, 3.0f, static_cast<u32>(LaneKind::Driving)},
                        {0, true, 1.6f, static_cast<u32>(LaneKind::Cycle)}};
    CurveNetwork cycles;
    use_lane_rules(cycles);
    const BuildResult c1 = traffic::build_road(cycles, straight({0, 0}, {200, 0}), with_cycle, 8.0f);
    const BuildResult c2 = traffic::build_road(cycles, straight({200, 0}, {400, 0}), with_cycle, 8.0f);
    CHECK(c1.ok && c2.ok);
    CHECK(!traffic::may_connect(cycles, TrackRef{c1.edges[0], 2}, TrackRef{c2.edges[0], 1}));
    CHECK(!traffic::may_connect(cycles, TrackRef{c1.edges[0], 1}, TrackRef{c2.edges[0], 2}));
    CHECK(traffic::may_connect(cycles, TrackRef{c1.edges[0], 1}, TrackRef{c2.edges[0], 1}));
}

/// At a crossroads the turns come out of the right lanes, and a pocket takes
/// its turn away from the through lanes.
void test_turn_lanes() {
    using traffic::LaneKind;
    CurveNetwork net;
    use_lane_rules(net);
    RoadSpec two;
    two.lanes = {{0, false, 3.25f, 0}, {0, false, 3.25f, 0}, {0, true, 3.25f, 0}, {0, true, 3.25f, 0}};
    traffic::build_road(net, straight({-200, 0}, {200, 0}), two, 8.0f);
    traffic::build_road(net, straight({0, -200}, {0, 200}), two, 8.0f);

    // Find the two arms of the crossroads that run west to east and north.
    u32 west = ~0u, north = ~0u;
    for (u32 i = 0; i < static_cast<u32>(net.edges().size()); ++i) {
        if (!net.valid_edge(i)) continue;
        const NetEdge& e = net.edge(i);
        const Vec2 mid = (net.node(e.a).pos + net.node(e.b).pos) * 0.5f;
        if (mid.x < -50 && std::fabs(mid.y) < 10) west = i;
        if (mid.y < -50 && std::fabs(mid.x) < 10) north = i;
    }
    CHECK(west != ~0u && north != ~0u);

    // Heading east on the western arm, lane 3 is the kerb lane and lane 2 the
    // one by the centre line. Only the kerb lane may turn right, only the
    // centre lane may turn left, and both may go straight on.
    usize left_from_centre = 0, left_from_kerb = 0;
    for (const rw::Connector& c : net.connectors()) {
        if (c.from.edge != west) continue;
        if (traffic::turn_sense(net, c.from, c.to) >= 0) continue;
        if (c.from.track == 2) ++left_from_centre;
        if (c.from.track == 3) ++left_from_kerb;
    }
    CHECK(left_from_centre > 0);
    CHECK(left_from_kerb == 0);

    // Give that arm a left turn pocket: the through lanes lose the left turn.
    NetEdge& arm = net.edge(west);
    arm.tracks.insert(arm.tracks.begin() + 2, rw::Track{0, true, 3.0f, static_cast<u32>(LaneKind::LeftTurn)});
    arm.width += 3.0f;
    float at = -arm.width * 0.5f;
    for (rw::Track& t : arm.tracks) {
        t.offset = at + t.width * 0.5f;
        at += t.width;
    }
    net.rebuild();
    usize from_pocket = 0, left_from_through = 0;
    for (const rw::Connector& c : net.connectors()) {
        if (c.from.edge != west || traffic::turn_sense(net, c.from, c.to) >= 0) continue;
        if (c.from.track == 2) ++from_pocket;
        else ++left_from_through;
    }
    CHECK(from_pocket > 0);
    CHECK(left_from_through == 0);
    // And the pocket does nothing but turn left.
    for (const rw::Connector& c : net.connectors())
        if (c.from.edge == west && c.from.track == 2) CHECK(traffic::turn_sense(net, c.from, c.to) < 0);
}

} // namespace

int main() {
    test_lane_rules();
    test_turn_lanes();
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
