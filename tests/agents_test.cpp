// Headless checks for network agents: following, spacing, stopping,
// yielding at conflicts, sideways changes, determinism and reflection.

#include <rewind/sim/network.h>
#include <rewind/sim/network_agents.h>
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

constexpr float kDt = 1.0f / 60;

/// A straight line of three nodes with one forward track: 0 -> 300 -> 600.
CurveNetwork make_line() {
    CurveNetwork net;
    const u32 a = net.add_node({0, 0}), b = net.add_node({300, 0}), c = net.add_node({600, 0});
    net.set_tracks(net.add_edge(a, b), 1, 0, 4);
    net.set_tracks(net.add_edge(b, c), 1, 0, 4);
    net.rebuild();
    return net;
}

CurveNetwork make_cross() {
    CurveNetwork net;
    const u32 c = net.add_node({0, 0}, 0, 0, 10);
    const u32 w = net.add_node({-200, 0}), e = net.add_node({200, 0}), n = net.add_node({0, -200}), s = net.add_node({0, 200});
    for (u32 other : {w, e, n, s}) net.set_tracks(net.add_edge(other, c), 1, 1, 4);
    net.rebuild();
    return net;
}

void run(NetworkAgents& agents, const CurveNetwork& net, int ticks) {
    for (int i = 0; i < ticks; ++i) agents.tick(net, kDt);
}

void test_single_agent() {
    CurveNetwork net = make_line();
    NetworkAgents agents;
    NetAgentParams p;
    p.max_speed = 100;
    const Route r = net.route(TrackRef{0, 0}, TrackRef{1, 0});
    CHECK(r.found && r.steps.size() == 3);
    const u32 a = agents.spawn(r, 0, p);
    CHECK(agents.count() == 1 && agents.valid(a));
    run(agents, net, 120);
    const NetAgent& ag = agents.agent(a);
    CHECK(ag.speed > 90 && ag.distance > 60); // at 60 units/s^2 it takes under two seconds to reach 100
    CHECK(agents.pose(net, a).position.x > 60 && std::fabs(agents.pose(net, a).position.y) < 3);
    CHECK(agents.current_step(a)->kind == RouteStep::Kind::Track);
    run(agents, net, 60 * 8);
    CHECK(ag.done && ag.speed == 0);
    CHECK(std::fabs(agents.pose(net, a).position.x - 600) < 1);

    // A stop distance on the last track holds it short of the end.
    const u32 b = agents.spawn(r, 0, p);
    agents.agent(b).stop_distance = 100;
    run(agents, net, 60 * 10);
    CHECK(agents.agent(b).done && std::fabs(agents.pose(net, b).position.x - 400) < 1);
    agents.remove(b);
    CHECK(!agents.valid(b) && agents.count() == 1);
}

void test_following() {
    CurveNetwork net = make_line();
    NetworkAgents agents;
    NetAgentParams p;
    p.max_speed = 100;
    p.length = 4;
    p.min_gap = 2;
    const Route r = net.route(TrackRef{0, 0}, TrackRef{1, 0});
    // A slow leader far ahead and a fast follower behind it.
    NetAgentParams slow = p;
    slow.max_speed = 20;
    const u32 leader = agents.spawn(r, 60, slow);
    const u32 follower = agents.spawn(r, 0, p);
    float min_gap = 1e9f;
    for (int i = 0; i < 60 * 6; ++i) {
        agents.tick(net, kDt);
        const NetAgent& l = agents.agent(leader);
        if (l.done) break;
        // Same track or the follower behind on the previous piece: measure along x.
        const float gap = agents.pose(net, leader).position.x - slow.length - agents.pose(net, follower).position.x;
        min_gap = std::min(min_gap, gap);
    }
    CHECK(min_gap >= p.min_gap - 0.5f);  // never rammed the leader
    CHECK(min_gap < 30);                 // but did close in on it
    CHECK(agents.agent(follower).speed <= slow.max_speed + 5);

    // A stopped agent blocks the one behind at the standing gap.
    NetworkAgents queue;
    NetAgentParams stopper = p;
    const Route only_first = net.route(TrackRef{0, 0}, TrackRef{0, 0});
    const u32 front = queue.spawn(only_first, 200, stopper);
    queue.agent(front).stop_distance = 200; // stop right here, on its last (only) track
    const u32 back = queue.spawn(r, 100, p);
    run(queue, net, 60 * 5);
    const float front_tail = queue.agent(front).distance - stopper.length;
    CHECK(queue.agent(back).speed < 0.1f);
    CHECK(std::fabs(front_tail - queue.agent(back).distance - p.min_gap) < 1.0f);
    CHECK(queue.on_track(0).size() == 2 && queue.on_track(0)[0] == front);
}

void test_conflicts() {
    CurveNetwork net = make_cross();
    NetworkAgents agents;
    NetAgentParams p;
    p.max_speed = 80;
    // West -> east straight, and north -> south straight: their connectors cross.
    const Route we = net.route(TrackRef{0, 1}, TrackRef{1, 0});
    const Route ns = net.route(TrackRef{2, 1}, TrackRef{3, 0});
    CHECK(we.found && ns.found);
    const u32 first = agents.spawn(we, 150, p); // 40 from the stop line
    const u32 second = agents.spawn(ns, 130, p);
    bool overlapped = false;
    bool waited = false;
    for (int i = 0; i < 60 * 8; ++i) {
        agents.tick(net, kDt);
        const RouteStep* a = agents.current_step(first);
        const RouteStep* b = agents.current_step(second);
        if (a && b && a->kind == RouteStep::Kind::Connector && b->kind == RouteStep::Kind::Connector) overlapped = true;
        if (agents.agent(second).waiting || agents.agent(first).waiting) waited = true;
    }
    CHECK(!overlapped); // never both in the crossing
    CHECK(waited);
    CHECK(agents.agent(first).done && agents.agent(second).done);

    // A rule can hold everyone: a red light.
    NetworkAgents held;
    held.set_entry_rule([](const NetworkAgents&, u32, u32) { return false; });
    const u32 stuck = held.spawn(we, 150, p);
    run(held, net, 60 * 5);
    CHECK(held.agent(stuck).waiting && held.agent(stuck).speed == 0);
    CHECK(std::fabs(held.agent(stuck).distance - net.track_length(TrackRef{0, 1})) < 0.5f);
    CHECK(held.current_step(stuck)->kind == RouteStep::Kind::Track);
    CHECK(held.on_connector(we.steps[1].index).empty());
}

void test_change() {
    CurveNetwork net;
    const u32 a = net.add_node({0, 0}), b = net.add_node({400, 0}), c = net.add_node({800, 0});
    const u32 e1 = net.add_edge(a, b), e2 = net.add_edge(b, c);
    net.set_tracks(e1, 2, 0, 4);
    net.set_tracks(e2, 2, 0, 4);
    net.set_connect_filter([](const CurveNetwork&, u32, TrackRef from, TrackRef to) { return from.edge != to.edge && from.track == to.track; });
    net.rebuild();
    const Route r = net.route(TrackRef{e1, 0}, TrackRef{e2, 1});
    CHECK(r.found);
    bool has_change = false;
    for (const RouteStep& s : r.steps) has_change = has_change || s.kind == RouteStep::Kind::Change;
    CHECK(has_change);
    NetworkAgents agents;
    NetAgentParams p;
    p.max_speed = 100;
    const u32 id = agents.spawn(r, 0, p);
    const float y0 = agents.pose(net, id).position.y;
    bool changing = false;
    float mid_y = y0;
    for (int i = 0; i < 60 * 12 && !agents.agent(id).done; ++i) {
        agents.tick(net, kDt);
        if (agents.current_step(id) && agents.current_step(id)->kind == RouteStep::Kind::Change) {
            changing = true;
            mid_y = agents.pose(net, id).position.y;
        }
    }
    CHECK(changing);
    CHECK(agents.agent(id).done);
    const float y1 = agents.pose(net, id).position.y;
    CHECK(std::fabs(y1 - y0 - 4) < 0.5f); // moved one track over
    CHECK(mid_y > y0 && mid_y < y1 + 0.5f);
    CHECK(std::fabs(agents.pose(net, id).position.x - 800) < 1);
}

void test_determinism_and_reflection() {
    CurveNetwork net = make_cross();
    NetAgentParams p;
    const Route we = net.route(TrackRef{0, 1}, TrackRef{1, 0});
    const Route ns = net.route(TrackRef{2, 1}, TrackRef{3, 0});
    NetworkAgents a, b;
    for (int i = 0; i < 4; ++i) {
        a.spawn(we, static_cast<float>(i) * 20, p);
        b.spawn(we, static_cast<float>(i) * 20, p);
        a.spawn(ns, static_cast<float>(i) * 25, p);
        b.spawn(ns, static_cast<float>(i) * 25, p);
    }
    run(a, net, 120);
    run(b, net, 120);
    for (u32 i = 0; i < 8; ++i) CHECK(a.agent(i).distance == b.agent(i).distance && a.agent(i).step == b.agent(i).step);

    // Save mid-run, continue both copies, compare.
    Writer w;
    {
        SaveVisitor save(w);
        a.reflect(save);
    }
    NetworkAgents c;
    {
        Reader r(w.bytes());
        LoadVisitor load(r);
        c.reflect(load);
    }
    CHECK(c.count() == a.count());
    run(a, net, 120);
    run(c, net, 120);
    for (u32 i = 0; i < 8; ++i) CHECK(a.agent(i).distance == c.agent(i).distance && a.agent(i).speed == c.agent(i).speed);
}

} // namespace

int main() {
    test_single_agent();
    test_following();
    test_conflicts();
    test_change();
    test_determinism_and_reflection();
    if (failures == 0) std::printf("agents_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
