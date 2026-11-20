// Headless checks for building roads: the shapes, the snapping, laying a
// road so that junctions form, and tidying up after edits.

#include "../game/roads/build.h"

#include <cmath>
#include <cstdio>

using namespace rw;
using namespace roads;

namespace {

int failures = 0;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

bool near(float a, float b, float eps = 0.05f) { return std::fabs(a - b) <= eps; }
bool near(Vec2 a, Vec2 b, float eps = 0.05f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps); }

CurveNetwork make_net() {
    CurveNetwork net;
    net.set_connect_filter(may_connect);
    net.set_track_trim(arm_trim);
    return net;
}

/// Lay a road and tidy up, as the game does on a click.
BuildResult build(CurveNetwork& net, const Path& path, u32 type = 0) {
    const BuildResult r = lay_road(net, path, type);
    refresh(net);
    return r;
}

usize alive_nodes(const CurveNetwork& net) {
    usize n = 0;
    for (u32 i = 0; i < net.nodes().size(); ++i) n += net.valid_node(i) ? 1 : 0;
    return n;
}
usize alive_edges(const CurveNetwork& net) {
    usize n = 0;
    for (u32 i = 0; i < net.edges().size(); ++i) n += net.valid_edge(i) ? 1 : 0;
    return n;
}

void test_shapes() {
    // An arc leaves along the heading it was given and reaches the point.
    float radius = 0;
    const Path arc = arc_from({0, 0}, {1, 0}, {50, 50}, &radius);
    CHECK(!arc.empty());
    CHECK(near(radius, 50));
    CHECK(near(arc.start(), {0, 0}) && near(arc.end(), {50, 50}));
    CHECK(near(arc.segments.front().direction(0), {1, 0}));
    CHECK(near(arc.length(), 50 * 3.14159265f * 0.5f, 0.3f));
    CHECK(near(tightest_radius(arc), 50, 0.6f));
    // In line already: a straight.
    CHECK(arc_from({0, 0}, {1, 0}, {40, 0}, &radius).segments.size() == 1 && radius == 0);
    CHECK(arc_from({0, 0}, {1, 0}, {-40, 0}).empty()); // straight back: no arc does that
    CHECK(tightest_radius(straight({0, 0}, {30, 5})) > 1e6f);

    // Joining two headings: no kink at either end, whatever lies between.
    const Vec2 east{1, 0}, north{0, -1};
    const Path s = connect({0, 0}, east, {80, 20}, east); // offset sideways: an S
    CHECK(near(s.segments.front().direction(0), east) && near(s.segments.back().direction(1), east));
    CHECK(near(s.start(), {0, 0}) && near(s.end(), {80, 20}));
    CHECK(tightest_radius(s) > 25);
    const Path bend = connect({0, 0}, east, {40, -40}, north); // a quarter turn
    CHECK(near(bend.segments.back().direction(1), north));
    CHECK(near(tightest_radius(bend), 40, 12));
    const Path hairpin = connect({0, 0}, east, {0, 30}, east * -1.0f); // facing back: a U
    CHECK(near(hairpin.segments.back().direction(1), east * -1.0f));
    CHECK(tightest_radius(hairpin) > 8);

    // Planning picks the shape from what the two ends know.
    Snap from, to;
    from.pos = {0, 0};
    to.pos = {60, 0};
    CHECK(plan_road(from, to, Shape::Straight, 7).ok);
    CHECK(plan_road(from, to, Shape::Straight, 7).radius == 0);
    to.pos = {3, 0};
    CHECK(!plan_road(from, to, Shape::Straight, 7).ok); // too short
    from.has_dir = true;
    from.dir = east;
    to.pos = {40, 40};
    const Plan curve = plan_road(from, to, Shape::Curve, 7);
    CHECK(curve.ok && near(curve.radius, 40, 0.6f) && !curve.joined);
    CHECK(plan_road(from, to, Shape::Straight, 7).radius == 0); // asked for a straight, got one
    to.pos = {6, 6};
    CHECK(!plan_road(from, to, Shape::Curve, 7).ok); // a bend nobody could drive
    // Both ends know their heading: joined, in either shape.
    to.pos = {90, 30};
    to.has_dir = true;
    to.dir = east * -1.0f; // an open end facing us: a road carried on from it would head west
    const Plan joined = plan_road(from, to, Shape::Straight, 7);
    CHECK(joined.ok && joined.joined);
    CHECK(near(joined.path.segments.back().direction(1), east)); // arrives heading into that road
}

void test_junctions_form() {
    CurveNetwork net = make_net();
    // One street, then another across it: a crossroads appears.
    CHECK(build(net, straight({-100, 0}, {100, 0})).ok);
    CHECK(alive_edges(net) == 1 && alive_nodes(net) == 2);
    const BuildResult across = build(net, straight({0, -80}, {0, 80}));
    CHECK(across.ok && across.edges.size() == 2);
    CHECK(alive_edges(net) == 4 && alive_nodes(net) == 5);
    const u32 centre = net.nearest_node({0, 0}, 1);
    CHECK(centre != kNone && net.edges_at(centre).size() == 4);
    CHECK(net.node(centre).radius > 6 && net.node(centre).radius < 12);
    // Lanes stop short of it and every arm can turn each way and go straight.
    CHECK(net.connectors_at(centre).size() == 12);
    CHECK(!net.conflicts().empty());
    for (u32 e : net.edges_at(centre)) CHECK(net.edge(e).tag == 1 && net.edge(e).tracks.size() == 2);

    // A road ending on another makes a T, splitting the one it lands on.
    CHECK(build(net, straight({60, 60}, {60, 0})).ok);
    const u32 tee = net.nearest_node({60, 0}, 1);
    CHECK(tee != kNone && net.edges_at(tee).size() == 3);
    CHECK(alive_edges(net) == 6);
    CHECK(net.connectors_at(tee).size() == 6);

    // Ending on an existing junction reuses it.
    CHECK(build(net, straight({-70, -70}, {0, -80})).ok);
    CHECK(net.edges_at(net.nearest_node({0, -80}, 1)).size() == 2);

    // A road too short, or from a junction back to itself, is refused and changes nothing.
    const usize before = alive_edges(net);
    CHECK(!build(net, straight({200, 200}, {203, 200})).ok);
    CHECK(alive_edges(net) == before);

    // Crossing two roads at once makes two junctions and three pieces.
    CurveNetwork grid = make_net();
    build(grid, straight({0, -60}, {0, 60}));
    build(grid, straight({50, -60}, {50, 60}));
    const BuildResult through = build(grid, straight({-40, 0}, {90, 0}), 1);
    CHECK(through.ok && through.edges.size() == 3);
    CHECK(grid.edges_at(grid.nearest_node({0, 0}, 1)).size() == 4);
    CHECK(grid.edges_at(grid.nearest_node({50, 0}, 1)).size() == 4);
    // The avenue is wider, so its junctions with the streets are too.
    CHECK(grid.node(grid.nearest_node({0, 0}, 1)).radius > net.node(centre).radius);
}

void test_trims() {
    // A side road at right angles runs up to the main road; at a sharp angle
    // it has to stop further back, and so does the road it meets.
    CurveNetwork square = make_net();
    build(square, straight({-100, 0}, {100, 0}));
    build(square, straight({0, 80}, {0, 0}));
    const u32 n = square.nearest_node({0, 0}, 1);
    float side = 0;
    for (u32 e : square.edges_at(n)) {
        const float trim = arm_trim(square, n, e);
        CHECK(trim > 6 && trim < 11);
        const NetEdge& edge = square.edge(e);
        const Vec2 far = square.node(edge.a == n ? edge.b : edge.a).pos;
        if (far.y > 10) side = trim;
    }
    CHECK(side > 0);

    CurveNetwork sharp = make_net();
    build(sharp, straight({-100, 0}, {100, 0}));
    build(sharp, straight({70, 40}, {0, 0})); // about thirty degrees
    const u32 m = sharp.nearest_node({0, 0}, 1);
    float longest = 0;
    for (u32 e : sharp.edges_at(m)) longest = std::max(longest, arm_trim(sharp, m, e));
    CHECK(longest > 15 && longest < 30);
    CHECK(near(sharp.node(m).radius, longest, 0.01f));

    // A wide road pushes a side road's stop further out than a narrow one does.
    CurveNetwork wide = make_net();
    build(wide, straight({-100, 0}, {100, 0}), 2); // six lanes
    build(wide, straight({0, 80}, {0, 0}));
    const u32 w = wide.nearest_node({0, 0}, 1);
    float wide_side = 0;
    for (u32 e : wide.edges_at(w)) {
        const NetEdge& edge = wide.edge(e);
        if (wide.node(edge.a == w ? edge.b : edge.a).pos.y > 10) wide_side = arm_trim(wide, w, e);
    }
    CHECK(wide_side > side + 5);

    // Two junctions close together share the road between them.
    CurveNetwork close = make_net();
    build(close, straight({-100, 0}, {100, 0}));
    build(close, straight({0, -60}, {0, 60}));
    build(close, straight({14, -60}, {14, 60}));
    const u32 a = close.nearest_node({0, 0}, 1), b = close.nearest_node({14, 0}, 1);
    for (u32 e : close.edges_at(a))
        if (close.edge(e).a == b || close.edge(e).b == b) {
            CHECK(arm_trim(close, a, e) + arm_trim(close, b, e) < 14);
            for (u32 t = 0; t < close.edge(e).tracks.size(); ++t) CHECK(close.track_length(TrackRef{e, t}) > 1.0f);
        }
}

void test_snapping() {
    CurveNetwork net = make_net();
    build(net, straight({0, 0}, {100, 0}));
    build(net, straight({50, 0}, {50, 80}));

    // Open ground: nothing to stick to.
    CHECK(snap_to_network(net, {200, 200}, 8, 6).kind == Snap::Free);
    // The middle of a road: a point on it, square under the cursor.
    const Snap mid = snap_to_network(net, {25, 3}, 8, 6);
    CHECK(mid.kind == Snap::Edge && near(mid.pos, {25, 0}) && !mid.has_dir);
    CHECK(snap_to_network(net, {25, 9}, 8, 6).kind == Snap::Free); // out of reach
    // A junction pulls from further off than a road does.
    const Snap node = snap_to_network(net, {55, 6}, 8, 6);
    CHECK(node.kind == Snap::Node && near(node.pos, {50, 0}));
    // An open end knows which way the road was heading.
    const Snap end = snap_to_network(net, {103, 2}, 8, 6);
    CHECK(end.kind == Snap::End && near(end.pos, {100, 0}) && end.has_dir && near(end.dir, {1, 0}));
    const Snap other = snap_to_network(net, {-2, 1}, 8, 6);
    CHECK(other.kind == Snap::End && near(other.dir, {-1, 0}));
    const Snap down = snap_to_network(net, {50, 82}, 8, 6);
    CHECK(down.kind == Snap::End && near(down.dir, {0, 1}));
    // Close to the end of a road, along it: that is the end too.
    CHECK(snap_to_network(net, {95, 0.5f}, 2, 6).kind == Snap::End);

    CHECK(near(square_onto(net, 0, {30, 40}), {30, 0}));
}

void test_carrying_on_and_joining() {
    // Carrying a road on from its open end, in line: one road, no junction.
    CurveNetwork net = make_net();
    build(net, straight({0, 0}, {60, 0}));
    const Snap end = snap_to_network(net, {60, 0}, 8, 6);
    Snap to;
    to.pos = {140, 0};
    const Plan on = plan_road(end, to, Shape::Curve, 7);
    CHECK(on.ok && on.radius == 0);
    build(net, on.path);
    CHECK(alive_edges(net) == 1 && alive_nodes(net) == 2);
    CHECK(near(net.edge(0).path.length(), 140, 0.2f));

    // Carried on round a bend that leaves without a kink: still one road.
    const Snap tip = snap_to_network(net, {140, 0}, 8, 6);
    to.pos = {200, 60};
    const Plan round = plan_road(tip, to, Shape::Curve, 7);
    CHECK(round.ok && near(round.radius, 60, 0.8f));
    build(net, round.path);
    CHECK(alive_edges(net) == 1);

    // Carried on with a corner: the corner stays, as a seam with room to turn.
    const Snap corner_from = snap_to_network(net, {200, 60}, 8, 6);
    to.pos = {260, 60};
    build(net, plan_road(corner_from, to, Shape::Straight, 7).path);
    CHECK(alive_edges(net) == 2);
    const u32 corner = net.nearest_node({200, 60}, 1);
    CHECK(net.edges_at(corner).size() == 2 && net.node(corner).radius > 3);
    CHECK(net.connectors_at(corner).size() == 2); // each lane carries on round it

    // Two open ends, offset: joined by an S that neither end can feel.
    CurveNetwork two = make_net();
    build(two, straight({0, 0}, {60, 0}));
    build(two, straight({160, 30}, {220, 30}));
    const Snap a = snap_to_network(two, {60, 0}, 8, 6), b = snap_to_network(two, {160, 30}, 8, 6);
    CHECK(a.kind == Snap::End && b.kind == Snap::End);
    const Plan s = plan_road(a, b, Shape::Straight, 7);
    CHECK(s.ok && s.joined && s.radius > 30);
    build(two, s.path);
    // No kink anywhere, same lanes throughout: it is one road now.
    CHECK(alive_edges(two) == 1 && alive_nodes(two) == 2);
    CHECK(two.route(TrackRef{0, 1}, TrackRef{0, 1}).found);

    // A different kind of road joined on: the seam stays, and has room for the lanes to change.
    CurveNetwork mixed = make_net();
    build(mixed, straight({0, 0}, {80, 0}), 0);
    const Snap street_end = snap_to_network(mixed, {80, 0}, 8, 6);
    to.pos = {180, 0};
    build(mixed, plan_road(street_end, to, Shape::Straight, 14).path, 1);
    CHECK(alive_edges(mixed) == 2);
    const u32 seam = mixed.nearest_node({80, 0}, 1);
    CHECK(mixed.node(seam).radius >= 13);
    CHECK(!mixed.connectors_at(seam).empty());
}

void test_editing() {
    CurveNetwork net = make_net();
    build(net, straight({-100, 0}, {100, 0}));
    build(net, straight({0, 80}, {0, 0}));
    const u32 tee = net.nearest_node({0, 0}, 1);
    CHECK(net.edges_at(tee).size() == 3 && alive_edges(net) == 3);

    // Taking the side road away leaves a road that simply carries on: the
    // junction goes with it, and so does the node at the far end.
    u32 side = kNone;
    for (u32 e : net.edges_at(tee)) {
        const NetEdge& edge = net.edge(e);
        if (net.node(edge.a == tee ? edge.b : edge.a).pos.y > 10) side = e;
    }
    CHECK(side != kNone);
    net.remove_edge(side);
    refresh(net);
    CHECK(alive_edges(net) == 1 && alive_nodes(net) == 2);
    u32 only = kNone;
    for (u32 e = 0; e < net.edges().size(); ++e)
        if (net.valid_edge(e)) only = e;
    CHECK(near(net.edge(only).path.length(), 200, 0.2f));
    CHECK(net.track_count() == 2);

    // Turned round, a road is the same road: its lanes still keep right.
    CurveNetwork one = make_net();
    build(one, straight({0, 0}, {100, 0}), 3); // one way, eastbound
    const Vec2 before = one.track_path(TrackRef{0, 0}).segments.front().direction(0);
    reverse_edge(one, 0);
    one.rebuild();
    CHECK(near(one.track_path(TrackRef{0, 0}).segments.front().direction(0), before));
    CHECK(near(one.node(one.edge(0).a).pos, {100, 0}));

    // Bending a road through a point keeps its ends where they were.
    CurveNetwork bendy = make_net();
    build(bendy, straight({0, 0}, {100, 0}));
    bend_through(bendy, 0, {50, 20});
    refresh(bendy);
    CHECK(near(bendy.edge(0).path.start(), {0, 0}) && near(bendy.edge(0).path.end(), {100, 0}));
    const PathSampler sampler(bendy.edge(0).path, 0.05f);
    CHECK(sampler.distance_to({50, 20}) < 0.1f);

    // A new kind of road on an existing one.
    apply_type(bendy, 0, 1);
    refresh(bendy);
    CHECK(bendy.edge(0).tracks.size() == 4 && bendy.edge(0).tag == 2);
    CHECK(near(bendy.edge(0).width, 14));
    apply_type(bendy, 0, 99); // no such type: nothing happens
    CHECK(bendy.edge(0).tracks.size() == 4);

    // Every type in the catalogue is as wide as its lanes.
    for (const RoadType& type : road_types()) CHECK(type.width() >= 3.5f && !type.lanes.empty());
}

/// The junction nearest a point.
u32 junction(const CurveNetwork& net, Vec2 at) { return net.nearest_node(at, 3); }
u32 arm_towards(const CurveNetwork& net, u32 node, Vec2 dir) {
    for (const Arm& arm : arms_of(net, node))
        if (dot(arm.dir, dir) > 0.9f) return arm.edge;
    return kNone;
}
usize movements(const CurveNetwork& net, u32 node, u32 from, u32 to) {
    usize count = 0;
    for (u32 c : net.connectors_at(node)) count += net.connectors()[c].from.edge == from && net.connectors()[c].to.edge == to ? 1 : 0;
    return count;
}

void test_priority() {
    enum { kStreet, kAvenue, kBoulevard, kOneWay, kOneWay2 };
    const Vec2 east{1, 0}, west{-1, 0}, north{0, -1}, south{0, 1};

    // A big road crossed by a small one has priority.
    CurveNetwork net = make_net();
    CHECK(build(net, straight({-80, 0}, {80, 0}), kAvenue).ok);
    CHECK(build(net, straight({0, -60}, {0, 60}), kStreet).ok);
    u32 at = junction(net, {0, 0});
    Priority p = priority_at(net, at);
    CHECK(!p.equals && !p.bends && p.edges.size() == 2);
    CHECK(p.has(arm_towards(net, at, east)) && p.has(arm_towards(net, at, west)) && !p.has(arm_towards(net, at, north)));

    // Two the same size: nobody has.
    net = make_net();
    CHECK(build(net, straight({-80, 0}, {80, 0})).ok);
    CHECK(build(net, straight({0, -60}, {0, 60})).ok);
    p = priority_at(net, junction(net, {0, 0}));
    CHECK(p.equals && p.edges.empty());

    // A side road never has, and the road it joins always does.
    net = make_net();
    CHECK(build(net, straight({-80, 0}, {80, 0})).ok);
    CHECK(build(net, straight({0, 60}, {0, 0}), kAvenue).ok);
    at = junction(net, {0, 0});
    p = priority_at(net, at);
    CHECK(!p.equals && !p.bends && p.edges.size() == 2 && !p.has(arm_towards(net, at, south)));

    // A fork with nothing to choose between its branches is a junction of equals.
    net = make_net();
    CHECK(build(net, straight({-80, 0}, {0, 0})).ok);
    CHECK(build(net, straight({0, 0}, {73, -27})).ok);
    CHECK(build(net, straight({0, 0}, {73, 27})).ok);
    p = priority_at(net, junction(net, {0, 0}));
    CHECK(p.equals && p.edges.empty());

    // The straighter branch of a fork is the main road.
    net = make_net();
    CHECK(build(net, straight({-80, 0}, {0, 0})).ok);
    CHECK(build(net, straight({0, 0}, {80, 0})).ok);
    CHECK(build(net, straight({0, 0}, {73, 27})).ok);
    at = junction(net, {0, 0});
    p = priority_at(net, at);
    CHECK(!p.equals && p.edges.size() == 2 && p.has(arm_towards(net, at, east)) && p.has(arm_towards(net, at, west)));

    // A big road that turns the corner keeps its priority and all its lanes.
    net = make_net();
    CHECK(build(net, straight({-80, 0}, {0, 0}), kAvenue).ok);
    CHECK(build(net, straight({0, 0}, {0, 60}), kAvenue).ok);
    CHECK(build(net, straight({0, 0}, {70, 0})).ok);
    CHECK(build(net, straight({0, 0}, {0, -60})).ok);
    at = junction(net, {0, 0});
    p = priority_at(net, at);
    const u32 w = arm_towards(net, at, west), s = arm_towards(net, at, south);
    CHECK(p.bends && !p.equals && p.edges.size() == 2 && p.has(w) && p.has(s));
    CHECK(movements(net, at, w, s) == 2 && movements(net, at, s, w) == 2);
    for (u32 c : net.connectors_at(at)) {
        const Connector& k = net.connectors()[c];
        CHECK(p.along(net, k) == ((k.from.edge == w && k.to.edge == s) || (k.from.edge == s && k.to.edge == w)));
    }
}

void test_lanes_at_junctions() {
    enum { kStreet, kAvenue, kBoulevard, kOneWay, kOneWay2 };
    // Where a road gains a lane, the new lane is fed: nothing begins from nowhere.
    CurveNetwork net = make_net();
    CHECK(build(net, straight({-80, 0}, {0, 0}), kStreet).ok);
    CHECK(build(net, straight({0, 0}, {80, 0}), kAvenue).ok);
    const u32 seam = junction(net, {0, 0});
    CHECK(net.edges_at(seam).size() == 2);
    for (u32 e : net.edges_at(seam))
        for (u32 t = 0; t < net.edge(e).tracks.size(); ++t) {
            const TrackRef lane{e, t};
            if (net.leaves_from(lane, seam)) CHECK(!net.connectors_into(lane).empty());
            if (net.arrives_at(lane, seam)) CHECK(!net.connectors_from(lane).empty());
        }

    // Two left-turn lanes turn side by side, each into a lane of its own.
    net = make_net();
    CHECK(build(net, straight({-80, 0}, {80, 0}), kBoulevard).ok);
    const BuildResult side = build(net, straight({0, 60}, {0, 0}), kOneWay2);
    CHECK(side.ok);
    for (Track& lane : net.edge(side.edges.back()).tracks) lane.tag = kLeftOnly;
    net.invalidate();
    refresh(net);
    const u32 tee = junction(net, {0, 0});
    std::vector<TrackRef> into;
    for (u32 c : net.connectors_at(tee)) {
        const Connector& k = net.connectors()[c];
        if (k.from.edge != side.edges.back()) continue;
        CHECK(turn_of(net, k.from, k.to) == -1);
        into.push_back(k.to);
    }
    CHECK(into.size() == 2 && !(into[0] == into[1]) && into[0].edge == into[1].edge);
}

void test_lane_editing() {
    enum { kStreet, kAvenue };
    const Vec2 east{1, 0}, west{-1, 0};
    // Two crossings on an avenue, a hundred metres apart.
    CurveNetwork net = make_net();
    CHECK(build(net, straight({-150, 0}, {150, 0}), kAvenue).ok);
    CHECK(build(net, straight({-50, -60}, {-50, 60}), kAvenue).ok);
    CHECK(build(net, straight({50, -60}, {50, 60}), kAvenue).ok);
    const u32 second = junction(net, {50, 0});
    const u32 middle = arm_towards(net, second, west);
    // The eastbound lanes of the piece in between, kerb lane first.
    std::vector<u32> lanes;
    for (u32 t = 0; t < net.edge(middle).tracks.size(); ++t)
        if (net.arrives_at(TrackRef{middle, t}, second)) lanes.push_back(t);
    CHECK(lanes.size() == 2);
    std::sort(lanes.begin(), lanes.end(), [&](u32 x, u32 y) { return lane_of(net.edge(middle), x).rank < lane_of(net.edge(middle), y).rank; });
    const TrackRef kerb{middle, lanes[0]}, centre{middle, lanes[1]};
    // By the rules: the kerb lane goes ahead and right, the centre lane ahead and left.
    CHECK(arrows_of(net, kerb) == (kStraight | kRight) && arrows_of(net, centre) == (kStraight | kLeft));

    // Painted: the centre lane turns left and nothing else.
    net.edge(middle).tracks[centre.track].tag = kLeft;
    net.invalidate();
    refresh(net);
    CHECK(arrows_of(net, centre) == kLeft && arrows_of(net, kerb) == (kStraight | kRight));
    // It runs the whole length of its road, so it has to be got into at the crossing before.
    CHECK(!net.connectors_into(centre).empty());

    // Both lanes left and ahead: two lanes turn left side by side, into a lane each.
    net.edge(middle).tracks[centre.track].tag = kLeft | kStraight;
    net.edge(middle).tracks[kerb.track].tag = kLeft | kStraight;
    net.invalidate();
    refresh(net);
    CHECK(arrows_of(net, centre) == (kLeft | kStraight) && arrows_of(net, kerb) == (kLeft | kStraight));
    std::vector<TrackRef> lefts;
    for (u32 t : lanes)
        for (u32 c : net.connectors_from(TrackRef{middle, t}))
            if (turn_of(net, net.connectors()[c].from, net.connectors()[c].to) == -1) lefts.push_back(net.connectors()[c].to);
    CHECK(lefts.size() == 2 && !(lefts[0] == lefts[1]));
    // Nobody may turn right any more: no lane has the arrow, and none is left to the rules.
    for (u32 t : lanes) CHECK((arrows_of(net, TrackRef{middle, t}) & kRight) == 0);

    // Lanes one at a time.
    net.edge(middle).tracks[centre.track].tag = net.edge(middle).tracks[kerb.track].tag = kAny;
    const bool way = net.edge(middle).tracks[kerb.track].forward;
    CHECK(lanes_one_way(net.edge(middle), way) == 2 && near(net.edge(middle).width, 14));
    CHECK(add_lane(net, middle, way));
    CHECK(lanes_one_way(net.edge(middle), way) == 3 && lanes_one_way(net.edge(middle), !way) == 2 && near(net.edge(middle).width, 21));
    CHECK(add_pocket(net, middle, way) && !add_pocket(net, middle, way)); // one pocket a side
    net.invalidate();
    refresh(net);
    u32 arriving = 0, right_turners = 0;
    for (u32 t = 0; t < net.edge(middle).tracks.size(); ++t) {
        if (!net.arrives_at(TrackRef{middle, t}, second)) continue;
        ++arriving;
        right_turners += (arrows_of(net, TrackRef{middle, t}) & kRight) ? 1 : 0;
    }
    CHECK(arriving == 4 && right_turners == 1); // three lanes and the pocket, which alone turns right
    CHECK(remove_pocket(net, middle, way) && remove_lane(net, middle, way) && remove_lane(net, middle, way) && remove_lane(net, middle, way));
    CHECK(lanes_one_way(net.edge(middle), way) == 0 && net.edge(middle).tracks.size() == 2); // one-way now
    CHECK(remove_lane(net, middle, !way) && !remove_lane(net, middle, !way)); // the last lane stays
    for (u32 i = 0; i < 10; ++i) add_lane(net, middle, !way);
    CHECK(lanes_one_way(net.edge(middle), !way) == kMostLanesOneWay);
}

void test_limits() {
    CurveNetwork net = make_net();
    CHECK(build(net, straight({-100, 0}, {100, 0})).ok);
    CHECK(*road_problem(net, straight({0, -60}, {0, 60}), 7) == 0);  // a square crossing
    CHECK(*road_problem(net, straight({0, 0}, {4, 0}), 7) != 0);      // too short
    CHECK(*road_problem(net, straight({-60, -12}, {60, 12}), 7) != 0); // crossing at a sliver of an angle
    CHECK(*road_problem(net, arc_from({0, -60}, {1, 0}, {5, -55}), 7) != 0); // a bend nobody could drive
    CHECK(min_radius_for(21) > min_radius_for(7));                    // a wider road needs a wider bend
    CHECK(build(net, straight({0, -60}, {0, 60})).ok);
    CHECK(*node_problem(net, junction(net, {0, 0})) == 0);
}

} // namespace

int main() {
    test_priority();
    test_lanes_at_junctions();
    test_limits();
    test_lane_editing();
    test_shapes();
    test_junctions_form();
    test_trims();
    test_snapping();
    test_carrying_on_and_joining();
    test_editing();
    if (failures == 0) std::printf("build_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
