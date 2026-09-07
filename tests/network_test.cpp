// Headless checks for the curve network: tracks, connectors, links,
// routing, conflicts, queries, splitting, reflection.

#include <rewind/sim/network.h>
#include <rewind/sim/serialize.h>

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
bool near(Vec2 a, Vec2 b, float eps = 1e-2f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps); }

/// A plus-shaped junction: four arms of 100 units meeting at the centre,
/// two tracks each way.
CurveNetwork make_cross() {
    CurveNetwork net;
    const u32 c = net.add_node({0, 0}, 0, 0, 8); // junction radius: tracks stop 8 short of the centre
    const u32 w = net.add_node({-100, 0}), e = net.add_node({100, 0}), n = net.add_node({0, -100}), s = net.add_node({0, 100});
    for (u32 other : {w, e, n, s}) {
        const u32 edge = net.add_edge(other, c);
        net.set_tracks(edge, 1, 1, 4);
    }
    net.rebuild();
    return net;
}

void test_tracks_and_connectors() {
    CurveNetwork net = make_cross();
    CHECK(net.nodes().size() == 5 && net.edges().size() == 4);
    CHECK(net.track_count() == 8);
    // Each edge: one track towards the centre, one away; offsets mirror.
    const NetEdge& e0 = net.edge(0);
    CHECK(e0.tracks.size() == 2 && e0.tracks[0].forward == false && e0.tracks[1].forward == true);
    CHECK(near(e0.tracks[0].offset, -2) && near(e0.tracks[1].offset, 2) && near(e0.width, 8));
    const TrackRef in{0, 1}; // west arm, towards the centre
    CHECK(near(net.track_length(in), 92));
    CHECK(near(net.track_path(in).start(), {-100, 2}) && near(net.track_path(in).end(), {-8, 2}));
    const TrackRef out{0, 0}; // west arm, away from the centre: reversed
    CHECK(near(net.track_path(out).start(), {-8, -2}) && near(net.track_path(out).end(), {-100, -2}));

    // At the centre every arriving track connects to the three other arms (no u-turn).
    CHECK(net.connectors_at(0).size() == 4 * 3);
    CHECK(net.connectors_from(in).size() == 3);
    for (const Connector& c : net.connectors()) {
        CHECK(near(c.path.start(), net.track_path(c.from).end()));
        CHECK(near(c.path.end(), net.track_path(c.to).start()));
        CHECK(c.length > 0);
    }
    // Dead ends have no connectors.
    CHECK(net.connectors_at(1).empty());
    // Crossing connectors at the junction are conflicts; merges are not.
    CHECK(!net.conflicts().empty());
    for (const Conflict& c : net.conflicts()) CHECK(c.node == 0);

    // A filter can forbid turns: only straight ahead.
    net.set_connect_filter([](const CurveNetwork& n, u32, TrackRef from, TrackRef to) {
        const Vec2 d0 = n.track_path(from).segments.back().direction(1);
        const Vec2 d1 = n.track_path(to).segments.front().direction(0);
        return dot(d0, d1) > 0.9f;
    });
    net.rebuild();
    CHECK(net.connectors_at(0).size() == 4);
    CHECK(net.conflicts().size() == 4); // each east-west stream crosses both north-south streams
}

void test_routing() {
    CurveNetwork net = make_cross();
    const TrackRef west_in{0, 1}, east_out{1, 0}, north_out{2, 0};
    Route r = net.route(west_in, east_out);
    CHECK(r.found && r.steps.size() == 3);
    CHECK(r.steps[0].kind == RouteStep::Kind::Track && r.steps[1].kind == RouteStep::Kind::Connector && r.steps[2].kind == RouteStep::Kind::Track);
    CHECK(near(r.cost, 92 + net.connectors()[r.steps[1].index].length, 0.5f));
    const Path p = net.route_path(r);
    CHECK(near(p.start(), {-100, 2}) && near(p.end(), {100, 2})); // keep right: outbound sits on +y too
    CHECK(near(p.length(), 184 + net.connectors()[r.steps[1].index].length, 1));

    // Partial: from 40 units in to 30 units before the end.
    Location from{west_in, 40}, to{east_out, 70};
    const Path partial = net.route_path(net.route(from, to), &from, &to);
    CHECK(near(partial.start(), {-60, 2}, 0.5f));
    CHECK(near(partial.end(), {78, 2}, 0.5f)); // 70 along a track that starts 8 from the centre

    // Turning left at the junction.
    Route left = net.route(west_in, north_out);
    CHECK(left.found && left.steps.size() == 3);
    // Unreachable: a track leading into a dead end back to the centre is fine, but no route out of nothing.
    CHECK(!net.route(TrackRef{9, 0}, east_out).found);
    // Same track: trivial.
    Route same = net.route(west_in, west_in);
    CHECK(same.found && same.steps.size() == 1);
}

void test_links_and_changes() {
    CurveNetwork net;
    const u32 a = net.add_node({0, 0}), b = net.add_node({300, 0}), c = net.add_node({600, 0});
    const u32 e1 = net.add_edge(a, b), e2 = net.add_edge(b, c);
    net.set_tracks(e1, 3, 0, 4); // three forward tracks
    net.set_tracks(e2, 3, 0, 4);
    // Only matching tracks continue across the node (the default would also
    // connect diagonally, which is the game's call).
    net.set_connect_filter([](const CurveNetwork&, u32, TrackRef from, TrackRef to) { return from.edge != to.edge && from.track == to.track; });
    net.rebuild();
    // Links per edge: track 0<->1 and 1<->2, both ways; no 0<->2.
    CHECK(net.links().size() == 8);
    CHECK(net.connectors().size() == 3);
    // Middle to middle straight: one connector.
    Route straight = net.route(TrackRef{e1, 1}, TrackRef{e2, 1});
    CHECK(straight.found && straight.steps.size() == 3);
    // Outer to outer across: cheapest is one sideways change somewhere, not two.
    net.change_penalty = 4;
    net.rebuild();
    Route across = net.route(TrackRef{e1, 0}, TrackRef{e2, 2});
    CHECK(across.found);
    int changes = 0;
    for (const RouteStep& s : across.steps) changes += s.kind == RouteStep::Kind::Change ? 1 : 0;
    CHECK(changes == 2);
    // With a huge penalty the route still exists (no other way) but costs more.
    const float cheap = across.cost;
    net.change_penalty = 1000;
    net.rebuild();
    Route pricey = net.route(TrackRef{e1, 0}, TrackRef{e2, 2});
    CHECK(pricey.found && pricey.cost > cheap + 1000);
    const Path path = net.route_path(across);
    CHECK(near(path.start(), net.track_path(TrackRef{e1, 0}).start()));
    CHECK(near(path.end(), net.track_path(TrackRef{e2, 2}).end()));
}

void test_geometry_and_edits() {
    CurveNetwork net;
    const u32 a = net.add_node({0, 0}, 0), b = net.add_node({200, 0}, 10);
    const u32 e = net.add_curved_edge(a, b, {1, -1}, {1, 1});
    net.set_tracks(e, 1, 1, 6);
    net.rebuild();
    CHECK(net.edge(e).path.segments.size() == 1);
    CHECK(near(net.elevation_at(e, 0.5f), 5));
    CHECK(net.track_length(TrackRef{e, 1}) > 200);

    float t = 0;
    CHECK(net.nearest_edge({100, -50}, 1e9f, &t) == e && t > 0.3f && t < 0.7f);
    CHECK(net.nearest_node({190, 5}) == b);
    CHECK(net.nearest_node({190, 5}, 5) == ~0u);
    Location loc;
    CHECK(net.locate({100, -60}, loc));
    CHECK(loc.track.edge == e);
    const PathPoint pp = net.point_at(loc);
    CHECK(distance(pp.position, {100, -60}) < 40);

    // Split the edge: the new node sits on the curve, both halves keep the tracks.
    const u32 mid = net.split_edge(e, 0, 0.5f);
    net.rebuild();
    CHECK(net.valid_node(mid) && net.edges().size() == 2);
    CHECK(near(net.node(mid).elevation, 5));
    CHECK(net.edges_at(mid).size() == 2);
    CHECK(net.edge(0).b == mid && net.edge(1).a == mid);
    CHECK(net.edge(1).tracks.size() == 2);
    // Straight through the new node is a route of one connector.
    Route through = net.route(TrackRef{0, 1}, TrackRef{1, 1});
    CHECK(through.found && through.steps.size() == 3);

    // Moving a node drags the edge ends along.
    net.move_node(mid, net.node(mid).pos + Vec2{0, 30});
    net.rebuild();
    CHECK(near(net.edge(0).path.end(), net.node(mid).pos));
    CHECK(near(net.edge(1).path.start(), net.node(mid).pos));

    // Removing the node kills its edges; the rest survives.
    net.remove_node(mid);
    net.rebuild();
    CHECK(!net.valid_edge(0) && !net.valid_edge(1) && net.track_count() == 0);
}

void test_reflection() {
    CurveNetwork net = make_cross();
    Writer w;
    {
        SaveVisitor save(w);
        net.reflect(save);
    }
    CurveNetwork loaded;
    {
        Reader r(w.bytes());
        LoadVisitor load(r);
        loaded.reflect(load);
    }
    CHECK(loaded.nodes().size() == 5 && loaded.track_count() == 8);
    CHECK(loaded.connectors().size() == net.connectors().size());
    CHECK(loaded.route(TrackRef{0, 1}, TrackRef{1, 0}).found);
}

} // namespace

void test_penalties_and_staleness() {
    // Two ways from west to east: straight through the centre, or around via north.
    CurveNetwork net = make_cross();
    const TrackRef west_in{0, 1}, east_out{1, 0};
    Route direct = net.route(west_in, east_out);
    CHECK(direct.found && direct.steps.size() == 3);
    CHECK(!net.route_affected(direct));
    const u64 v = net.version();

    // Closing the east arm's outbound track leaves no other way there; a
    // penalty on the connector's target makes the route pricier, not gone.
    net.set_track_penalty(east_out, 500);
    CHECK(net.version() > v);
    CHECK(near(net.track_penalty(east_out), 500));
    CHECK(net.route_affected(direct)); // one of its tracks changed after planning
    Route pricey = net.route(west_in, east_out);
    CHECK(pricey.found && pricey.cost > direct.cost + 400);
    CHECK(!net.route_affected(pricey));

    // A route that never touches the penalised track stays valid.
    Route north = net.route(west_in, TrackRef{2, 0});
    net.set_track_penalty(TrackRef{3, 0}, 50);
    CHECK(!net.route_affected(north));
    // A rebuild invalidates everything planned before it.
    net.rebuild();
    CHECK(net.route_affected(north) && net.route_affected(pricey));
    CHECK(near(net.track_penalty(east_out), 500)); // penalties survive the rebuild
    net.clear_track_penalties();
    CHECK(near(net.route(west_in, east_out).cost, direct.cost, 1e-3f));

    // The cache serves repeated questions; bidirectional gives the same answers.
    net.cache().clear();
    net.route(west_in, east_out);
    net.route(west_in, east_out);
    CHECK(net.cache().hits() >= 1);
    net.bidirectional = true;
    net.rebuild();
    Route bi = net.route(west_in, east_out);
    CHECK(bi.found && near(bi.cost, direct.cost, 1e-3f) && bi.steps.size() == 3);
}

/// Saving is a read: it must not rebuild the network, because the timeline
/// saves every tick to fingerprint it.
void test_save_does_not_rebuild() {
    CurveNetwork net = make_cross();
    const u64 version = net.version();
    Writer w;
    {
        SaveVisitor save(w);
        net.reflect(save);
    }
    CHECK(net.version() == version);
    CHECK(!SaveVisitor(w).writes());

    // Loading is a write: it must rebuild, or the derived data is stale.
    CurveNetwork loaded;
    {
        Reader r(w.bytes());
        LoadVisitor load(r);
        loaded.reflect(load);
    }
    CHECK(loaded.track_count() == net.track_count());
    CHECK(!loaded.connectors().empty());
}

int main() {
    test_save_does_not_rebuild();
    test_penalties_and_staleness();
    test_tracks_and_connectors();
    test_routing();
    test_links_and_changes();
    test_geometry_and_edits();
    test_reflection();
    if (failures == 0) std::printf("network_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
