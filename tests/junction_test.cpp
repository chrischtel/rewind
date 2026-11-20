// Headless checks for authored junctions and signals: turns a node carries
// itself, their groups, the lookup tables, merge conflicts, and signal plans
// with their timing.

#include <rewind/sim/network.h>
#include <rewind/sim/serialize.h>
#include <rewind/sim/signal.h>

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

/// A plus-shaped junction, one track each way. Edges 0..3 are the west,
/// east, north and south arms, each running towards the centre (node 0), so
/// track 1 of an arm arrives there and track 0 leaves.
CurveNetwork make_cross() {
    CurveNetwork net;
    const u32 c = net.add_node({0, 0}, 0, 0, 8);
    const u32 w = net.add_node({-100, 0}), e = net.add_node({100, 0}), n = net.add_node({0, -100}), s = net.add_node({0, 100});
    for (u32 other : {w, e, n, s}) {
        const u32 edge = net.add_edge(other, c);
        net.set_tracks(edge, 1, 1, 4);
    }
    net.rebuild();
    return net;
}

TrackRef in(u32 arm) { return TrackRef{arm, 1}; }
TrackRef out(u32 arm) { return TrackRef{arm, 0}; }

void test_authoring() {
    CurveNetwork net = make_cross();
    CHECK(net.arrives_at(in(0), 0) && !net.arrives_at(out(0), 0));
    CHECK(net.leaves_from(out(0), 0) && !net.leaves_from(in(0), 0));
    CHECK(!net.arrives_at(TrackRef{99, 0}, 0) && !net.arrives_at(TrackRef{0, 9}, 0));

    // Taking a node over changes nothing on the ground.
    CHECK(!net.node(0).authored);
    net.author_node(0);
    CHECK(net.node(0).authored && net.node(0).turns.size() == 12);
    net.rebuild();
    CHECK(net.connectors_at(0).size() == 12);

    // Forbid west to north; everything else stays.
    CHECK(net.find_connector(in(0), out(2)) != kNone);
    CHECK(net.allow_turn(0, in(0), out(2), false));
    net.rebuild();
    CHECK(net.connectors_at(0).size() == 11);
    CHECK(net.find_connector(in(0), out(2)) == kNone);
    CHECK(net.find_connector(in(0), out(1)) != kNone);
    CHECK(net.connectors_from(in(0)).size() == 2);
    CHECK(!net.route(in(0), out(2)).found); // no other way round on a single cross

    // Allow it again, and a u-turn the filter would never have made.
    CHECK(net.allow_turn(0, in(0), out(2), true));
    CHECK(net.allow_turn(0, in(0), out(0), true));
    CHECK(net.allow_turn(0, in(0), out(0), true)); // twice is still once
    net.rebuild();
    CHECK(net.connectors_at(0).size() == 13);
    CHECK(net.route(in(0), out(0)).found);

    // Not a movement of this node: refused, and nothing is authored by it.
    CHECK(!net.allow_turn(0, out(0), out(1)));
    CHECK(!net.allow_turn(0, in(0), in(1)));
    CHECK(!net.allow_turn(1, in(0), out(1)));
    CHECK(!net.node(1).authored);

    // Group, tag and priority travel from the turn to the connector.
    Turn* turn = net.find_turn(0, in(2), out(3));
    CHECK(turn != nullptr);
    if (turn) {
        turn->group = 7;
        turn->tag = 42;
        turn->priority = 3;
    }
    net.rebuild();
    const u32 c = net.find_connector(in(2), out(3));
    CHECK(c != kNone);
    if (c != kNone) {
        const Connector& k = net.connectors()[c];
        CHECK(k.group == 7 && k.tag == 42 && k.priority == 3 && k.node == 0);
    }
    CHECK(net.find_turn(0, in(2), out(2)) == nullptr);

    // Authored turns are state: they survive a save and a load.
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
    CHECK(loaded.node(0).authored && loaded.node(0).turns.size() == 13);
    CHECK(loaded.connectors_at(0).size() == 13);
    const u32 lc = loaded.find_connector(in(2), out(3));
    CHECK(lc != kNone && loaded.connectors()[lc].group == 7);

    // Handing the node back derives everything again.
    net.release_node(0);
    net.rebuild();
    CHECK(!net.node(0).authored && net.node(0).turns.empty());
    CHECK(net.connectors_at(0).size() == 12);
    CHECK(net.connectors()[net.find_connector(in(2), out(3))].group == 0);
}

void test_edits_under_authored_nodes() {
    // Splitting a road gives its far half a new index; the turns follow it.
    CurveNetwork net = make_cross();
    net.author_node(0);
    net.find_turn(0, in(0), out(1))->group = 5;
    const u32 mid = net.split_edge(0, 0, 0.5f);
    CHECK(mid != kNone);
    net.rebuild();
    CHECK(net.edges().size() == 5);
    CHECK(net.node(0).turns.size() == 12);
    CHECK(net.connectors_at(0).size() == 12);
    // The west arm at the centre is edge 4 now.
    CHECK(net.arrives_at(TrackRef{4, 1}, 0));
    const u32 c = net.find_connector(TrackRef{4, 1}, out(1));
    CHECK(c != kNone && net.connectors()[c].group == 5);
    // The seam in the middle is an ordinary derived node: straight through each way.
    CHECK(net.connectors_at(mid).size() == 2);
    CHECK(net.route(TrackRef{0, 1}, out(1)).found);

    // Removing a road drops the turns that used it, and only those.
    net.remove_edge(3);
    net.rebuild();
    CHECK(net.node(0).turns.size() == 6);
    CHECK(net.connectors_at(0).size() == 6);
    for (const Turn& t : net.node(0).turns) CHECK(t.from.edge != 3 && t.to.edge != 3);

    // Relaning a road keeps what still exists.
    net.set_tracks(1, 2, 1, 4); // east arm: two tracks arriving now (tracks 1 and 2), one leaving (0)
    net.rebuild();
    CHECK(net.node(0).turns.size() == 6);
    CHECK(net.allow_turn(0, TrackRef{1, 2}, out(2)));
    net.rebuild();
    CHECK(net.connectors_from(TrackRef{1, 2}).size() == 1);
}

void test_lookups_and_conflicts() {
    CurveNetwork net = make_cross();
    // Three roads feed every track that leaves the centre.
    for (u32 arm = 0; arm < 4; ++arm) {
        CHECK(net.connectors_into(out(arm)).size() == 3);
        CHECK(net.connectors_from(in(arm)).size() == 3);
        CHECK(net.connectors_into(in(arm)).empty()); // starts at a dead end
    }
    CHECK(net.connectors_at(99).empty());
    CHECK(net.connectors_from(TrackRef{99, 0}).empty());
    CHECK(net.conflicts_of(9999).empty());

    // Every conflict is listed under both of its connectors, with the
    // meeting point somewhere along each.
    const usize crossings = net.conflicts().size();
    CHECK(crossings > 0);
    usize listed = 0;
    for (u32 c = 0; c < net.connectors().size(); ++c)
        for (u32 index : net.conflicts_of(c)) {
            const Conflict& k = net.conflicts()[index];
            CHECK(k.connector_a == c || k.connector_b == c);
            CHECK(k.kind == ConflictKind::Crossing);
            ++listed;
        }
    CHECK(listed == crossings * 2);
    for (const Conflict& k : net.conflicts()) {
        const Connector& a = net.connectors()[k.connector_a];
        const Connector& b = net.connectors()[k.connector_b];
        CHECK(k.at_a >= 0 && k.at_a <= a.length + 0.5f);
        CHECK(k.at_b >= 0 && k.at_b <= b.length + 0.5f);
        CHECK(k.connector_a < k.connector_b);
    }
    // The two straight-through streams east-west and north-south cross in the
    // middle of the junction: half way along each connector.
    const u32 we = net.find_connector(in(0), out(1)), ns = net.find_connector(in(2), out(3));
    bool found = false;
    for (u32 index : net.conflicts_of(we)) {
        const Conflict& k = net.conflicts()[index];
        if (k.connector_a != ns && k.connector_b != ns) continue;
        found = true;
        const float along_we = k.connector_a == we ? k.at_a : k.at_b;
        CHECK(near(along_we, net.connectors()[we].length * 0.5f - 2.0f, 0.6f)); // 2 = the lane offset
        CHECK(near(k.where.x, -2, 0.3f) && near(k.where.y, 2, 0.3f));
    }
    CHECK(found);

    // Merges are listed on request: three feeders per leaving track, so three
    // pairs each, and the crossings stay what they were.
    net.merge_conflicts = true;
    net.rebuild();
    usize merges = 0, cross = 0;
    for (const Conflict& k : net.conflicts()) {
        if (k.kind == ConflictKind::Merge) {
            ++merges;
            CHECK(net.connectors()[k.connector_a].to == net.connectors()[k.connector_b].to);
            CHECK(near(k.at_a, net.connectors()[k.connector_a].length));
        } else ++cross;
    }
    CHECK(merges == 12);
    CHECK(cross == crossings);
}

void test_groups_and_plans() {
    CurveNetwork net = make_cross();
    CHECK(assign_turn_groups(net, 0, TurnGrouping::Approach) == 4);
    net.rebuild();
    for (u32 c : net.connectors_at(0)) CHECK(net.connectors()[c].group == net.connectors()[c].from.edge + 1);

    // With every turn allowed, each approach conflicts with every other one:
    // a left turn crosses the oncoming straight. Four phases, one each.
    CHECK(conflicting_groups(net, 0).size() == 6);
    SignalPlan plan = signal_plan_for(net, 0, 15);
    CHECK(plan.phases.size() == 4);
    for (const SignalPhase& p : plan.phases) CHECK(p.groups.size() == 1 && near(p.green, 15));
    CHECK(signal_plan_clash(plan, conflicting_groups(net, 0)) == kNone);

    // Straight on only: opposite approaches no longer conflict, two phases.
    net.release_node(0);
    net.set_connect_filter([](const CurveNetwork& n, u32, TrackRef from, TrackRef to) {
        const Vec2 d0 = n.track_path(from).segments.back().direction(1);
        const Vec2 d1 = n.track_path(to).segments.front().direction(0);
        return dot(d0, d1) > 0.9f;
    });
    net.rebuild();
    CHECK(assign_turn_groups(net, 0, TurnGrouping::Approach) == 4);
    net.rebuild();
    const auto conflicts = conflicting_groups(net, 0);
    CHECK(conflicts.size() == 4);
    plan = signal_plan_for(net, 0);
    CHECK(plan.phases.size() == 2);
    if (plan.phases.size() == 2) {
        CHECK(plan.phases[0].groups == (std::vector<u32>{1, 2})); // west and east
        CHECK(plan.phases[1].groups == (std::vector<u32>{3, 4})); // north and south
    }
    CHECK(signal_plan_clash(plan, conflicts) == kNone);
    // A plan that lets west and north go together is caught.
    SignalPlan bad = plan;
    bad.phases[0].groups = {1, 3};
    CHECK(signal_plan_clash(bad, conflicts) == 0);

    // Finer groupings.
    CurveNetwork fine = make_cross();
    CHECK(assign_turn_groups(fine, 0, TurnGrouping::Movement) == 12);
    CurveNetwork lanes = make_cross();
    CHECK(assign_turn_groups(lanes, 0, TurnGrouping::Lane) == 4);

    // A right turn that conflicts with nothing rides along in other phases.
    // West may only turn right (to the south arm... in y-down, towards +y is
    // right of travelling east); north-south run straight.
    CurveNetwork ride = make_cross();
    ride.author_node(0);
    ride.node(0).turns.clear();
    ride.node(0).turns.push_back(Turn{in(2), out(3), 1, 0, 0}); // north to south
    ride.node(0).turns.push_back(Turn{in(3), out(2), 1, 0, 0}); // south to north
    ride.node(0).turns.push_back(Turn{in(1), out(0), 2, 0, 0}); // east to west: crosses both
    ride.node(0).turns.push_back(Turn{in(0), out(3), 3, 0, 0}); // west turning into the south arm
    ride.merge_conflicts = false;
    ride.rebuild();
    const SignalPlan rp = signal_plan_for(ride, 0);
    CHECK(rp.phases.size() == 2);
    CHECK(signal_plan_clash(rp, conflicting_groups(ride, 0)) == kNone);
    // Group 3 conflicts with nobody (merging is not crossing), so it is in both.
    usize with3 = 0;
    for (const SignalPhase& p : rp.phases) with3 += p.has(3) ? 1 : 0;
    CHECK(with3 == 2);
}

void run(const SignalPlan& plan, SignalState& state, float seconds, bool demand = false) {
    const int steps = static_cast<int>(std::lround(seconds * 10));
    for (int i = 0; i < steps; ++i) signal_tick(plan, state, 0.1f, demand);
}

void test_signal_timing() {
    SignalPlan plan;
    plan.phases.push_back(SignalPhase{{1}, 10, 0});
    plan.phases.push_back(SignalPhase{{2}, 5, 0});
    plan.amber = 3;
    plan.all_red = 2;
    plan.red_amber = 1;
    CHECK(near(plan.cycle(), 25));

    SignalState s;
    CHECK(signal_aspect(plan, s, 1) == Aspect::Green && signal_aspect(plan, s, 2) == Aspect::Red);
    CHECK(signal_aspect(plan, s, 9) == Aspect::Red); // in no phase
    CHECK(near(signal_time_to_green(plan, s, 2), 15));
    CHECK(near(signal_time_to_green(plan, s, 1), 0));
    CHECK(signal_time_to_green(plan, s, 9) < 0);
    CHECK(near(signal_green_left(plan, s, 1), 10) && near(signal_green_left(plan, s, 2), 0));

    run(plan, s, 10.5f); // 10.5: green over, amber
    CHECK(signal_aspect(plan, s, 1) == Aspect::Amber && signal_aspect(plan, s, 2) == Aspect::Red);
    run(plan, s, 3.0f); // 13.5: all red
    CHECK(signal_aspect(plan, s, 1) == Aspect::Red && signal_aspect(plan, s, 2) == Aspect::Red);
    CHECK(near(signal_time_to_green(plan, s, 2), 1.5f, 0.05f));
    run(plan, s, 1.0f); // 14.5: the next group is told to get ready
    CHECK(signal_aspect(plan, s, 2) == Aspect::RedAmber && signal_aspect(plan, s, 1) == Aspect::Red);
    CHECK(!signal_is_go(signal_aspect(plan, s, 2)));
    run(plan, s, 1.0f); // 15.5: second phase
    CHECK(s.phase == 1);
    CHECK(signal_aspect(plan, s, 2) == Aspect::Green && signal_aspect(plan, s, 1) == Aspect::Red);
    run(plan, s, 10.0f); // 25.5: round again
    CHECK(s.phase == 0 && signal_aspect(plan, s, 1) == Aspect::Green);

    // The clock gives the same answer as ticking does.
    const SignalState at = signal_state_at(plan, 25.5f);
    CHECK(at.phase == 0 && near(at.time, 0.5f, 0.02f));
    CHECK(signal_state_at(plan, 16).phase == 1);
    // An offset shifts the whole cycle: this junction starts five seconds late.
    plan.offset = 5;
    CHECK(signal_state_at(plan, 5).phase == 0 && near(signal_state_at(plan, 5).time, 0));
    CHECK(signal_state_at(plan, 0).phase == 1); // still in the tail of the cycle before
    CHECK(signal_aspect(plan, signal_state_at(plan, 21), 2) == Aspect::Green);

    // No red-amber: straight from red to green.
    plan.offset = 0;
    plan.red_amber = 0;
    CHECK(signal_aspect(plan, signal_state_at(plan, 14.5f), 2) == Aspect::Red);

    // A group in two phases running one after the other keeps its green.
    SignalPlan overlap;
    overlap.phases.push_back(SignalPhase{{1, 3}, 10, 0});
    overlap.phases.push_back(SignalPhase{{2, 3}, 10, 0});
    overlap.phases.push_back(SignalPhase{{4}, 10, 0});
    const SignalState between = signal_state_at(overlap, 12);
    CHECK(signal_aspect(overlap, between, 3) == Aspect::Green);
    CHECK(signal_aspect(overlap, between, 1) == Aspect::Amber);
    CHECK(near(signal_green_left(overlap, SignalState{}, 3), 10 + 5 + 10));
    CHECK(signal_aspect(overlap, signal_state_at(overlap, 27), 3) == Aspect::Amber);

    // An empty plan does nothing and shows red.
    SignalPlan none;
    SignalState idle;
    signal_tick(none, idle, 1.0f);
    CHECK(idle.phase == 0 && signal_aspect(none, idle, 1) == Aspect::Red);
    CHECK(signal_state_at(none, 100).phase == 0);
}

void test_actuated() {
    SignalPlan plan;
    plan.phases.push_back(SignalPhase{{1}, 5, 12}); // at least 5, at most 12
    plan.phases.push_back(SignalPhase{{2}, 5, 0});
    plan.amber = 3;
    plan.all_red = 2;

    // Nobody waiting for more: the minimum.
    SignalState s;
    run(plan, s, 5.5f, false);
    CHECK(signal_aspect(plan, s, 1) == Aspect::Amber);

    // Demand all the way: green holds to the maximum, then gives way.
    s = SignalState{};
    run(plan, s, 11.5f, true);
    CHECK(signal_aspect(plan, s, 1) == Aspect::Green);
    run(plan, s, 1.0f, true); // 12.5
    CHECK(signal_aspect(plan, s, 1) == Aspect::Amber);
    run(plan, s, 5.0f, true); // 17.5: the fixed phase is not stretched by demand
    CHECK(s.phase == 1 && near(s.extended, 0));
    run(plan, s, 5.0f, true); // 22.5
    CHECK(signal_aspect(plan, s, 2) == Aspect::Amber);

    // Demand that stops: green ends where it stopped.
    s = SignalState{};
    run(plan, s, 8.0f, true);
    CHECK(signal_aspect(plan, s, 1) == Aspect::Green);
    run(plan, s, 0.5f, false);
    CHECK(signal_aspect(plan, s, 1) == Aspect::Amber);

    // Same inputs, same state: bit for bit.
    SignalState a, b;
    for (int i = 0; i < 1000; ++i) {
        signal_tick(plan, a, 1.0f / 60, i % 7 < 3);
        signal_tick(plan, b, 1.0f / 60, i % 7 < 3);
    }
    CHECK(a.phase == b.phase && a.time == b.time && a.extended == b.extended);
}

} // namespace

int main() {
    test_authoring();
    test_edits_under_authored_nodes();
    test_lookups_and_conflicts();
    test_groups_and_plans();
    test_signal_timing();
    test_actuated();
    if (failures == 0) std::printf("junction_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
