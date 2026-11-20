// Headless checks for two things the network does behind an edit: working
// out again only what the edit could have changed, and tracks that run only
// part of an edge.

#include <rewind/sim/network.h>
#include <rewind/sim/serialize.h>

#include <chrono>
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

constexpr u32 kNone = ~0u;

/// A square grid of streets, `n` junctions a side.
CurveNetwork make_grid(int n, float spacing = 80) {
    CurveNetwork net;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) net.add_node({static_cast<float>(x) * spacing, static_cast<float>(y) * spacing}, 0, 0, 8);
    const auto at = [n](int x, int y) { return static_cast<u32>(y * n + x); };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            if (x + 1 < n) net.set_tracks(net.add_edge(at(x, y), at(x + 1, y)), 1, 1, 3.5f);
            if (y + 1 < n) net.set_tracks(net.add_edge(at(x, y), at(x, y + 1)), 1, 1, 3.5f);
        }
    net.rebuild();
    return net;
}

bool same_point(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }

/// Is everything the network derived exactly what a network built from
/// scratch would have derived? Exactly: carried-over data is a copy, and
/// recomputed data comes from the same arithmetic.
bool same_derived(const CurveNetwork& a, const CurveNetwork& b) {
    if (a.track_count() != b.track_count()) return false;
    for (u32 i = 0; i < a.track_count(); ++i) {
        const TrackRef r = a.track_ref(i);
        if (!(r == b.track_ref(i))) return false;
        if (a.track_length(r) != b.track_length(r) || a.track_start(r) != b.track_start(r)) return false;
        if (!same_point(a.track_path(r).start(), b.track_path(r).start())) return false;
        if (!same_point(a.track_path(r).end(), b.track_path(r).end())) return false;
    }
    if (a.connectors().size() != b.connectors().size()) return false;
    for (usize i = 0; i < a.connectors().size(); ++i) {
        const Connector& p = a.connectors()[i];
        const Connector& q = b.connectors()[i];
        if (p.node != q.node || !(p.from == q.from) || !(p.to == q.to) || p.length != q.length || p.group != q.group)
            return false;
    }
    if (a.conflicts().size() != b.conflicts().size()) return false;
    for (usize i = 0; i < a.conflicts().size(); ++i) {
        const Conflict& p = a.conflicts()[i];
        const Conflict& q = b.conflicts()[i];
        if (p.node != q.node || p.connector_a != q.connector_a || p.connector_b != q.connector_b || p.kind != q.kind)
            return false;
        if (!same_point(p.where, q.where) || p.at_a != q.at_a || p.at_b != q.at_b) return false;
    }
    if (a.links().size() != b.links().size()) return false;
    for (usize i = 0; i < a.links().size(); ++i)
        if (!(a.links()[i].from == b.links()[i].from) || !(a.links()[i].to == b.links()[i].to)) return false;
    // And the tables agree with the lists.
    for (u32 c = 0; c < a.connectors().size(); ++c)
        if (a.conflicts_of(c) != b.conflicts_of(c)) return false;
    for (u32 n = 0; n < a.nodes().size(); ++n)
        if (a.connectors_at(n) != b.connectors_at(n) || a.edges_at(n) != b.edges_at(n)) return false;
    return true;
}

/// Rebuild after an edit, and compare with a copy that starts from nothing.
bool rebuild_and_compare(CurveNetwork& net) {
    net.rebuild();
    CurveNetwork fresh = net;
    fresh.invalidate();
    fresh.rebuild();
    return same_derived(net, fresh);
}

void test_incremental() {
    CurveNetwork net = make_grid(6);
    CHECK(net.last_rebuild().everything);
    const u32 edges = net.last_rebuild().edges, nodes = net.last_rebuild().nodes;
    CHECK(edges == 60 && nodes == 36);
    CHECK(net.last_rebuild().edges_rebuilt == edges && net.last_rebuild().nodes_rebuilt == nodes);

    // Nothing changed: nothing is worked out again, and the version still
    // moves on, as it always did.
    const u64 version = net.version();
    CHECK(rebuild_and_compare(net));
    CHECK(!net.last_rebuild().everything);
    CHECK(net.last_rebuild().edges_rebuilt == 0 && net.last_rebuild().nodes_rebuilt == 0);
    CHECK(net.version() > version);

    // One street in the middle gets a lane: only it, the streets sharing its
    // two junctions, and the junctions at their ends are touched.
    const u32 street = 27;
    CHECK(net.edges_at(net.edge(street).a).size() == 4 && net.edges_at(net.edge(street).b).size() == 4);
    net.set_tracks(street, 2, 1, 3.5f);
    CHECK(rebuild_and_compare(net));
    CHECK(net.last_rebuild().edges_rebuilt == 7);
    CHECK(net.last_rebuild().nodes_rebuilt == 8);

    // The same holds however the edit is made. Written straight into a node:
    net.node(14).radius = 12;
    CHECK(rebuild_and_compare(net));
    CHECK(net.last_rebuild().edges_rebuilt == 4 && net.last_rebuild().nodes_rebuilt == 5);
    // Straight into a track:
    net.edge(3).tracks[0].width = 3.0f;
    CHECK(rebuild_and_compare(net));
    CHECK(net.last_rebuild().edges_rebuilt > 0 && net.last_rebuild().edges_rebuilt <= 7);

    // Moving a junction drags its streets with it.
    // Moving a junction drags its streets with it, and a street that swings
    // changes the corner it makes at its far end: those junctions' other
    // streets are looked at again too. Still a neighbourhood, not the town.
    net.move_node(20, net.node(20).pos + Vec2{9, -6});
    CHECK(rebuild_and_compare(net));
    CHECK(net.last_rebuild().edges_rebuilt >= 4 && net.last_rebuild().edges_rebuilt <= 16);

    // A new street off the edge of the grid.
    const u32 out = net.add_node({-90, 40});
    const u32 spur = net.add_edge(out, 0);
    net.set_tracks(spur, 1, 1, 3.5f);
    CHECK(rebuild_and_compare(net));
    CHECK(net.last_rebuild().edges_rebuilt == 3 && net.last_rebuild().nodes_rebuilt == 4);

    // Bulldozing one, and then a whole junction.
    net.remove_edge(40);
    CHECK(rebuild_and_compare(net));
    CHECK(net.last_rebuild().edges_rebuilt <= 6);
    net.remove_node(28);
    CHECK(rebuild_and_compare(net));

    // Splitting a street puts a new junction in the middle of it.
    const u32 mid = net.split_edge(10, 0, 0.5f);
    CHECK(mid != kNone);
    CHECK(rebuild_and_compare(net));
    CHECK(net.last_rebuild().edges_rebuilt < 12);

    // A junction the player edits.
    const u32 junction = 15;
    TrackRef in{}, turn{};
    for (u32 c : net.connectors_at(junction)) {
        in = net.connectors()[c].from;
        turn = net.connectors()[c].to;
        break;
    }
    CHECK(net.allow_turn(junction, in, turn, false));
    CHECK(rebuild_and_compare(net));
    CHECK(net.last_rebuild().edges_rebuilt == 4);
    CHECK(net.find_connector(in, turn) == kNone);
    net.find_turn(junction, net.node(junction).turns[0].from, net.node(junction).turns[0].to)->group = 4;
    CHECK(rebuild_and_compare(net));

    // A setting that bears on every junction makes everything fresh.
    net.merge_conflicts = true;
    CHECK(rebuild_and_compare(net));
    CHECK(net.last_rebuild().everything);
    // So does a new rule for which track may join which.
    net.set_connect_filter([](const CurveNetwork& n, u32, TrackRef from, TrackRef to) {
        if (from.edge == to.edge) return false;
        const Vec2 d0 = n.track_path(from).segments.back().direction(1);
        const Vec2 d1 = n.track_path(to).segments.front().direction(0);
        return dot(d0, d1) > -0.1f; // no left... no turning back
    });
    CHECK(rebuild_and_compare(net));
    CHECK(net.last_rebuild().everything);
    net.set_tracks(5, 1, 2, 3.5f);
    CHECK(rebuild_and_compare(net));
    CHECK(!net.last_rebuild().everything && net.last_rebuild().edges_rebuilt <= 7);

    // Routes are planned on what was carried over as well as on what was not.
    CHECK(net.route(TrackRef{0, 1}, TrackRef{59, 1}).found);

    // Loading a saved network into one that already holds it costs nothing;
    // into an empty one, everything.
    Writer w;
    {
        SaveVisitor save(w);
        net.reflect(save);
    }
    {
        Reader r(w.bytes());
        LoadVisitor load(r);
        net.reflect(load); // this is what rewinding does
    }
    CHECK(net.last_rebuild().edges_rebuilt == 0 && net.last_rebuild().nodes_rebuilt == 0);
    CurveNetwork loaded;
    loaded.merge_conflicts = true;
    {
        Reader r(w.bytes());
        LoadVisitor load(r);
        loaded.reflect(load);
    }
    // No filter on the loaded one, so compare it with a copy without one too.
    CurveNetwork plain = net;
    plain.set_connect_filter(nullptr);
    plain.rebuild();
    CHECK(same_derived(loaded, plain));

    // And clearing leaves nothing behind.
    net.clear();
    CHECK(net.track_count() == 0 && net.connectors().empty() && net.conflicts().empty());
    CHECK(net.edges_at(0).empty());
}

void test_scale() {
    // A town of 1600 junctions: one edit should touch a handful of them.
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    CurveNetwork net = make_grid(40);
    const auto t1 = clock::now();
    CHECK(net.last_rebuild().edges == 3120 && net.last_rebuild().nodes == 1600);

    net.set_tracks(1500, 2, 2, 3.5f);
    const auto t2 = clock::now();
    net.rebuild();
    const auto t3 = clock::now();
    CHECK(net.last_rebuild().edges_rebuilt >= 1 && net.last_rebuild().edges_rebuilt <= 7);
    CHECK(net.last_rebuild().nodes_rebuilt >= 2 && net.last_rebuild().nodes_rebuilt <= 8);

    CurveNetwork fresh = net;
    fresh.invalidate();
    fresh.rebuild();
    CHECK(same_derived(net, fresh));

    const auto t4 = clock::now();
    fresh.invalidate();
    fresh.rebuild();
    const auto t5 = clock::now();
    (void)t0;
    (void)t1;
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::printf("rebuild_test: 3120 edges, everything %.1f ms, one edit %.2f ms\n", ms(t4, t5), ms(t2, t3));
}

void test_partial_tracks() {
    // A road with a pocket that opens 60 before its far junction, and beyond
    // that junction a road whose second lane is dropped 80 before its end.
    CurveNetwork net;
    const u32 a = net.add_node({0, 0}), b = net.add_node({200, 0}, 0, 0, 8), c = net.add_node({500, 0});
    const u32 e1 = net.add_edge(a, b), e2 = net.add_edge(b, c);
    net.set_tracks(e1, 2, 0, 3.5f);
    net.set_tracks(e2, 2, 0, 3.5f);
    net.edge(e1).tracks[1].begin = 140;
    net.edge(e1).tracks[1].taper = 20;
    net.edge(e2).tracks[1].end = 80;
    net.edge(e2).tracks[1].taper = 30;
    CHECK(!net.edge(e1).tracks[1].whole() && net.edge(e1).tracks[0].whole());
    net.rebuild();

    const TrackRef through{e1, 0}, pocket{e1, 1}, on{e2, 0}, dropped{e2, 1};
    // The pocket starts where it says and reaches the junction.
    CHECK(!net.leaves_from(pocket, a) && net.arrives_at(pocket, b));
    CHECK(near(net.track_start(pocket), 140) && near(net.track_start(through), 0));
    CHECK(near(net.track_length(pocket), 200 - 140 - 8));
    CHECK(near(net.track_path(pocket).start().x, 140) && near(net.track_path(pocket).end().x, 192));
    // Nothing feeds it at its open end: the only way in is sideways.
    CHECK(net.connectors_into(pocket).empty());
    CHECK(net.connectors_from(pocket).size() == 2);
    bool linked = false;
    for (const TrackLink& l : net.links()) linked = linked || (l.from == through && l.to == pocket);
    CHECK(linked);
    const Route into = net.route(through, pocket);
    CHECK(into.found && into.steps.size() == 3 && into.steps[1].kind == RouteStep::Kind::Change);

    // The same place on the track beside it, where there is one.
    float beside = 0;
    CHECK(net.beside(through, 150, pocket, beside) && near(beside, 10));
    CHECK(!net.beside(through, 100, pocket, beside));
    CHECK(net.beside(pocket, 10, through, beside) && near(beside, 150));
    CHECK(!net.beside(through, 10, on, beside)); // another edge altogether

    // The dropped lane leaves the junction but never arrives anywhere.
    CHECK(net.leaves_from(dropped, b) && !net.arrives_at(dropped, c));
    CHECK(near(net.track_length(dropped), 300 - 8 - 80));
    CHECK(net.connectors_from(dropped).empty());
    CHECK(net.connectors_into(dropped).size() == 2);
    // Whoever is on it has to move over to get anywhere.
    const Route off = net.route(dropped, on);
    CHECK(off.found && off.steps.size() == 3 && off.steps[1].kind == RouteStep::Kind::Change);

    // Two partial tracks that never run side by side are not linked.
    CurveNetwork apart;
    const u32 p = apart.add_node({0, 0}), q = apart.add_node({300, 0});
    const u32 e = apart.add_edge(p, q);
    apart.set_tracks(e, 3, 0, 3.5f);
    apart.edge(e).tracks[0].end = 200;   // only the first hundred
    apart.edge(e).tracks[2].begin = 200; // only the last hundred
    apart.rebuild();
    for (const TrackLink& l : apart.links()) {
        CHECK(!(l.from == TrackRef{e, 0} && l.to == TrackRef{e, 2}));
        CHECK(!(l.from == TrackRef{e, 2} && l.to == TrackRef{e, 0}));
    }
    CHECK(apart.links().size() == 4); // each with the whole lane between them, both ways

    // Splitting before the pocket: it goes to the far piece, measured afresh.
    CurveNetwork cut = net;
    cut.author_node(b);
    const usize turns = cut.node(b).turns.size();
    const u32 mid = cut.split_edge(e1, 0, 0.5f);
    cut.rebuild();
    const u32 tail = static_cast<u32>(cut.edges().size() - 1);
    CHECK(cut.edge(e1).tracks.size() == 1 && cut.edge(tail).tracks.size() == 2);
    CHECK(near(cut.edge(tail).tracks[1].begin, 40) && near(cut.edge(tail).tracks[1].end, 0));
    CHECK(cut.arrives_at(TrackRef{tail, 1}, b));
    CHECK(cut.connectors_at(mid).size() == 1); // one lane through the new seam
    // The junction's own turns followed the road to its new number.
    CHECK(cut.node(b).turns.size() == turns);
    for (const Turn& t : cut.node(b).turns) CHECK(t.from.edge == tail);
    CHECK(cut.find_connector(TrackRef{tail, 1}, on) != kNone);

    // Splitting through the pocket: both pieces carry part of it, and each
    // part now runs up to the new junction.
    CurveNetwork through_it = net;
    const u32 mid2 = through_it.split_edge(e1, 0, 0.85f); // at 170
    through_it.rebuild();
    const u32 tail2 = static_cast<u32>(through_it.edges().size() - 1);
    CHECK(through_it.edge(e1).tracks.size() == 2 && through_it.edge(tail2).tracks.size() == 2);
    CHECK(near(through_it.edge(e1).tracks[1].begin, 140) && near(through_it.edge(e1).tracks[1].end, 0));
    CHECK(near(through_it.edge(tail2).tracks[1].begin, 0));
    CHECK(through_it.arrives_at(TrackRef{e1, 1}, mid2) && through_it.leaves_from(TrackRef{tail2, 1}, mid2));

    // A partial track survives a save.
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
    CHECK(near(loaded.edge(e1).tracks[1].begin, 140) && near(loaded.edge(e1).tracks[1].taper, 20));
    CHECK(same_derived(net, loaded));
}

} // namespace

int main() {
    test_incremental();
    test_scale();
    test_partial_tracks();
    if (failures == 0) std::printf("rebuild_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
