// Headless checks for how traffic moves: the Intelligent Driver Model, and
// changing tracks along a track with a gap to move into.

#include <rewind/sim/network.h>
#include <rewind/sim/network_agents.h>

#include <algorithm>
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

constexpr float kDt = 1.0f / 60;
constexpr float kLane = 3.5f;

/// A car, in metres and seconds.
NetAgentParams car() {
    NetAgentParams p;
    p.model = FollowModel::Idm;
    p.length = 4.5f;
    p.max_speed = 14; // about 50 km/h
    p.accel = 1.5f;
    p.decel = 2.5f;
    p.min_gap = 2;
    p.headway = 1.2f;
    p.change_length = 20;
    return p;
}

/// Take everyone who has arrived off the road, as a game does: one that has
/// parked at the end of its route still takes up room there.
int clear_arrived(NetworkAgents& agents) {
    int n = 0;
    for (u32 i = 0; i < agents.agents().size(); ++i)
        if (agents.valid(i) && agents.agent(i).done) {
            agents.remove(i);
            ++n;
        }
    return n;
}

void run(NetworkAgents& agents, const CurveNetwork& net, float seconds) {
    const int ticks = static_cast<int>(std::lround(seconds / kDt));
    for (int i = 0; i < ticks; ++i) agents.tick(net, kDt);
}

/// The closest any two agents' fronts come. Two cars nose to tail in one
/// lane are a car length and a gap apart; two side by side in neighbouring
/// lanes are a lane apart. Anything much under a lane is two cars in one place.
float closest_pair(const NetworkAgents& agents, const CurveNetwork& net) {
    std::vector<Vec2> at;
    for (u32 i = 0; i < agents.agents().size(); ++i)
        if (agents.valid(i)) at.push_back(agents.pose(net, i).position);
    float best = 1e9f;
    for (usize i = 0; i < at.size(); ++i)
        for (usize j = i + 1; j < at.size(); ++j) best = std::min(best, distance(at[i], at[j]));
    return best;
}

void test_free_road_and_stopping() {
    CurveNetwork net;
    const u32 a = net.add_node({0, 0}), b = net.add_node({600, 0});
    const u32 e = net.add_edge(a, b);
    net.set_tracks(e, 1, 0, kLane);
    net.rebuild();
    const Route r = net.route(TrackRef{e, 0}, TrackRef{e, 0});
    NetworkAgents agents;
    const u32 id = agents.spawn(r, 0, car());

    // Pulls away at its full acceleration, which then fades: never a jump.
    float top = 0, hardest = 0, strongest = 0, last_speed = 0;
    bool smooth = true;
    for (int i = 0; i < 60 * 20; ++i) {
        agents.tick(net, kDt);
        const NetAgent& c = agents.agent(id);
        top = std::max(top, c.speed);
        strongest = std::max(strongest, c.accel);
        hardest = std::min(hardest, c.accel);
        if (std::fabs(c.speed - last_speed) > 2.6f * kDt + 1e-4f) smooth = false;
        last_speed = c.speed;
    }
    CHECK(smooth);
    CHECK(strongest > 1.3f && strongest <= 1.5f + 1e-3f);
    CHECK(top > 13.0f && top <= 14.0f + 1e-3f); // approaches the speed it wants and never passes it
    CHECK(hardest > -0.01f);                   // nothing to brake for yet

    // Comes to rest at the end of its route: on it, not past it, not short of it.
    run(agents, net, 60);
    const NetAgent& c = agents.agent(id);
    CHECK(c.done && c.speed == 0);
    CHECK(std::fabs(c.distance - 600) < 0.01f);
}

void test_queue_and_wave() {
    // A road, a junction that is shut, a road beyond. Eight cars arrive and queue.
    CurveNetwork net;
    const u32 a = net.add_node({0, 0}), b = net.add_node({500, 0}, 0, 0, 6), c = net.add_node({900, 0});
    const u32 e1 = net.add_edge(a, b), e2 = net.add_edge(b, c);
    net.set_tracks(e1, 1, 0, kLane);
    net.set_tracks(e2, 1, 0, kLane);
    net.rebuild();
    const Route r = net.route(TrackRef{e1, 0}, TrackRef{e2, 0});
    CHECK(r.found);

    NetworkAgents agents;
    bool open = false;
    agents.set_entry_rule([&](const NetworkAgents&, u32, u32) { return open; });
    constexpr int kCars = 8;
    std::vector<u32> cars;
    for (int i = 0; i < kCars; ++i) cars.push_back(agents.spawn(r, 300.0f - static_cast<float>(i) * 30, car()));

    float min_gap_seen = 1e9f, hardest = 0;
    bool braked = false;
    for (int t = 0; t < 60 * 60; ++t) {
        agents.tick(net, kDt);
        for (int i = 1; i < kCars; ++i) {
            const NetAgent& lead = agents.agent(cars[static_cast<usize>(i - 1)]);
            const NetAgent& me = agents.agent(cars[static_cast<usize>(i)]);
            min_gap_seen = std::min(min_gap_seen, lead.distance - lead.params.length - me.distance);
        }
        for (u32 id : cars) {
            hardest = std::min(hardest, agents.agent(id).accel);
            braked = braked || agents.agent(id).accel < -0.3f;
        }
    }
    const float line = net.track_length(TrackRef{e1, 0});
    // The first is on the line; the rest stand a gap apart behind it; nobody
    // ever touched, and nobody had to stand on the brakes to manage it.
    const NetAgent& first = agents.agent(cars[0]);
    CHECK(first.waiting && first.speed == 0);
    CHECK(first.distance <= line + 1e-3f && first.distance > line - 0.3f);
    for (int i = 1; i < kCars; ++i) {
        const NetAgent& lead = agents.agent(cars[static_cast<usize>(i - 1)]);
        const NetAgent& me = agents.agent(cars[static_cast<usize>(i)]);
        const float gap = lead.distance - lead.params.length - me.distance;
        CHECK(me.speed < 0.05f);
        CHECK(gap > 1.9f && gap < 3.0f);
    }
    CHECK(min_gap_seen > 1.0f);
    CHECK(braked);
    CHECK(hardest > -2.5f * 3 - 1e-3f); // within the hardest braking a car has
    CHECK(hardest > -4.0f);              // and in fact nowhere near it: the queue eased to a stand

    // The lead says who is in front and how far.
    const NetworkAgents::Lead lead = agents.lead_of(net, cars[3]);
    CHECK(lead.agent == cars[2] && lead.gap > 1.9f && lead.gap < 3.0f);
    CHECK(agents.lead_of(net, cars[0]).agent == ~0u);

    // Open the junction: they pull away one after another, front to back.
    open = true;
    std::vector<int> started(kCars, -1);
    int arrived = 0;
    for (int t = 0; t < 60 * 120; ++t) {
        agents.tick(net, kDt);
        for (int i = 0; i < kCars; ++i)
            if (started[static_cast<usize>(i)] < 0 && agents.valid(cars[static_cast<usize>(i)]) &&
                agents.agent(cars[static_cast<usize>(i)]).speed > 1.0f)
                started[static_cast<usize>(i)] = t;
        arrived += clear_arrived(agents);
    }
    for (int i = 0; i < kCars; ++i) CHECK(started[static_cast<usize>(i)] >= 0);
    for (int i = 1; i < kCars; ++i) CHECK(started[static_cast<usize>(i)] > started[static_cast<usize>(i - 1)]);
    // About a second a car: the wave that runs back through any queue.
    const float per_car = static_cast<float>(started[kCars - 1] - started[0]) * kDt / (kCars - 1);
    CHECK(per_car > 0.4f && per_car < 2.5f);
    CHECK(arrived == kCars);
}

void test_models_side_by_side() {
    // Both models on one road: each keeps its own way of driving, neither
    // runs into the other.
    CurveNetwork net;
    const u32 a = net.add_node({0, 0}), b = net.add_node({800, 0});
    const u32 e = net.add_edge(a, b);
    net.set_tracks(e, 1, 0, kLane);
    net.rebuild();
    const Route r = net.route(TrackRef{e, 0}, TrackRef{e, 0});
    NetworkAgents agents;
    NetAgentParams simple = car();
    simple.model = FollowModel::Simple;
    simple.max_speed = 8;
    const u32 slow = agents.spawn(r, 60, simple);
    const u32 fast = agents.spawn(r, 0, car());
    float closest = 1e9f;
    int slow_at = -1, fast_at = -1;
    for (int t = 0; t < 60 * 300 && (slow_at < 0 || fast_at < 0); ++t) {
        agents.tick(net, kDt);
        if (slow_at < 0 && fast_at < 0)
            closest = std::min(closest, agents.agent(slow).distance - 4.5f - agents.agent(fast).distance);
        if (slow_at < 0 && agents.agent(slow).done) slow_at = t;
        if (fast_at < 0 && agents.agent(fast).done) fast_at = t;
        clear_arrived(agents);
    }
    CHECK(slow_at >= 0 && fast_at >= 0);
    CHECK(closest > 1.0f);
    CHECK(fast_at > slow_at); // it could not overtake in one lane

    // A rule that takes the drive away still stops an Idm car, and gives it back.
    NetworkAgents hill;
    bool stalled = true;
    hill.set_motion_rule([&](const NetworkAgents&, const CurveNetwork&, u32, MotionLimits m) {
        if (stalled) m.accel = -1.0f;
        return m;
    });
    const u32 lorry = hill.spawn(r, 0, car());
    hill.agent(lorry).speed = 10;
    run(hill, net, 15);
    CHECK(hill.agent(lorry).speed == 0 && !hill.agent(lorry).done);
    stalled = false;
    run(hill, net, 10);
    CHECK(hill.agent(lorry).speed > 5);
}

/// Two lanes one way, then two more; a lane leads only into its own.
CurveNetwork make_two_lanes(u32& e1, u32& e2) {
    CurveNetwork net;
    const u32 a = net.add_node({0, 0}), b = net.add_node({600, 0}), c = net.add_node({900, 0});
    e1 = net.add_edge(a, b);
    e2 = net.add_edge(b, c);
    net.set_tracks(e1, 2, 0, kLane);
    net.set_tracks(e2, 2, 0, kLane);
    net.set_connect_filter([](const CurveNetwork&, u32, TrackRef from, TrackRef to) { return from.edge != to.edge && from.track == to.track; });
    net.rebuild();
    return net;
}

void test_change_along_the_track() {
    u32 e1 = 0, e2 = 0;
    const CurveNetwork net = make_two_lanes(e1, e2);
    const Route across = net.route(TrackRef{e1, 0}, TrackRef{e1, 1});
    CHECK(across.found && across.steps.size() == 3 && across.steps[1].kind == RouteStep::Kind::Change);

    // An empty road: it moves over straight away, not at the far end, and
    // keeps driving while it does.
    {
        NetworkAgents agents;
        const u32 id = agents.spawn(across, 0, car());
        const float y0 = agents.pose(net, id).position.y;
        float began = -1, ended = -1, slowest_while = 1e9f;
        for (int t = 0; t < 60 * 120 && !agents.agent(id).done; ++t) {
            agents.tick(net, kDt);
            const RouteStep* step = agents.current_step(id);
            if (step && step->kind == RouteStep::Kind::Change) {
                if (began < 0) began = agents.agent(id).change_start;
                if (t > 120) slowest_while = std::min(slowest_while, agents.agent(id).speed);
            } else if (began >= 0 && ended < 0) {
                ended = agents.agent(id).distance;
            }
        }
        CHECK(began >= 0 && began < 5);
        CHECK(ended > 19 && ended < 30); // one change length further on
        CHECK(agents.agent(id).done);
        CHECK(std::fabs(agents.pose(net, id).position.y - y0 - kLane) < 0.1f);
        CHECK(std::fabs(agents.agent(id).distance - 600) < 0.01f);
    }

    // A slower car alongside in the lane it wants: it waits until it is
    // clear ahead of that car, and only then moves over.
    {
        NetworkAgents agents;
        NetAgentParams crawl = car();
        crawl.max_speed = 4;
        const Route stay = net.route(TrackRef{e1, 1}, TrackRef{e1, 1});
        const u32 slow = agents.spawn(stay, 30, crawl);
        const u32 id = agents.spawn(across, 28, car());
        agents.agent(slow).speed = 4;
        agents.agent(id).speed = 14; // already at speed, so it is the one drawing ahead
        float clear_by = -1e9f, closest = 1e9f;
        bool changed = false;
        for (int t = 0; t < 60 * 200 && !agents.agent(id).done; ++t) {
            agents.tick(net, kDt);
            closest = std::min(closest, closest_pair(agents, net));
            const RouteStep* step = agents.current_step(id);
            if (!changed && step && step->kind == RouteStep::Kind::Change) {
                changed = true;
                // Its tail, against the slow car's front, at the moment it went.
                clear_by = agents.agent(id).change_start - 4.5f - agents.agent(slow).distance;
            }
        }
        CHECK(changed);
        CHECK(clear_by >= 2.0f - 0.2f); // at least the standing gap ahead of it
        CHECK(closest > 3.0f);          // side by side is a lane apart; never closer
        CHECK(agents.agent(id).done);
    }

    // A car coming up fast behind in the wanted lane: the gap has to be
    // bigger than for one keeping pace, so it lets that car past first.
    {
        NetworkAgents agents;
        const Route stay = net.route(TrackRef{e1, 1}, TrackRef{e1, 1});
        NetAgentParams dawdle = car();
        dawdle.max_speed = 5;
        const u32 id = agents.spawn(across, 40, dawdle);
        const u32 fast = agents.spawn(stay, 26, car()); // 9.5 behind its tail: enough for a slow one, not for this
        agents.agent(fast).speed = 14;
        agents.agent(id).speed = 5;
        agents.tick(net, kDt);
        CHECK(agents.current_step(id)->kind == RouteStep::Kind::Track); // not yet
        bool changed = false;
        float fast_ahead_by = 0, closest = 1e9f;
        for (int t = 0; t < 60 * 200 && !agents.agent(id).done; ++t) {
            agents.tick(net, kDt);
            closest = std::min(closest, closest_pair(agents, net));
            const RouteStep* step = agents.current_step(id);
            if (!changed && step && step->kind == RouteStep::Kind::Change) {
                changed = true;
                fast_ahead_by = agents.agent(fast).distance - agents.agent(id).change_start;
            }
        }
        CHECK(changed && fast_ahead_by > 4.5f); // the fast one was through
        CHECK(closest > 3.0f);
    }
}

void test_pocket_and_lane_drop() {
    // A pocket that opens 150 before the junction: nobody can be in it before it begins.
    {
        CurveNetwork net;
        const u32 a = net.add_node({0, 0}), b = net.add_node({600, 0}, 0, 0, 8), c = net.add_node({900, 0});
        const u32 e1 = net.add_edge(a, b), e2 = net.add_edge(b, c);
        net.set_tracks(e1, 2, 0, kLane);
        net.set_tracks(e2, 1, 0, kLane);
        net.edge(e1).tracks[1].begin = 450;
        net.edge(e1).tracks[1].taper = 30;
        net.rebuild();
        const TrackRef through{e1, 0}, pocket{e1, 1};
        Route r = net.route(through, pocket);
        CHECK(r.found && r.steps.size() == 3);
        NetworkAgents agents;
        const u32 id = agents.spawn(r, 0, car());
        float began = -1;
        for (int t = 0; t < 60 * 200 && !agents.agent(id).done; ++t) {
            agents.tick(net, kDt);
            const RouteStep* step = agents.current_step(id);
            if (began < 0 && step && step->kind == RouteStep::Kind::Change) began = agents.agent(id).change_start;
        }
        CHECK(began >= 450 - 0.5f && began < 470); // as soon as there was a pocket to move into
        CHECK(agents.agent(id).done);
        // Parked at the end of the pocket, a lane over from where it started.
        CHECK(std::fabs(agents.pose(net, id).position.x - 592) < 0.2f);
        CHECK(std::fabs(agents.pose(net, id).position.y - kLane * 0.5f) < 0.1f);
    }

    // A lane that ends: everyone on it merges before it does, taking turns
    // with the lane that carries on, and nobody is left behind.
    {
        CurveNetwork net;
        const u32 a = net.add_node({0, 0}), b = net.add_node({600, 0}, 0, 0, 8), c = net.add_node({1000, 0});
        const u32 e1 = net.add_edge(a, b), e2 = net.add_edge(b, c);
        net.set_tracks(e1, 2, 0, kLane);
        net.set_tracks(e2, 1, 0, kLane);
        net.edge(e1).tracks[1].end = 200; // gone 200 before the junction
        net.edge(e1).tracks[1].taper = 40;
        net.rebuild();
        const TrackRef keeps{e1, 0}, ends{e1, 1}, on{e2, 0};
        const Route straight = net.route(keeps, on), merging = net.route(ends, on);
        CHECK(straight.found && merging.found);
        bool has_change = false;
        for (const RouteStep& s : merging.steps) has_change = has_change || s.kind == RouteStep::Kind::Change;
        CHECK(has_change);

        NetworkAgents agents;
        constexpr int kEach = 7;
        std::vector<u32> lane_a, lane_b;
        // Both lanes full and close up, so the merge has to be taken in turns.
        for (int i = 0; i < kEach; ++i) {
            lane_a.push_back(agents.spawn(straight, 330.0f - static_cast<float>(i) * 9, car()));
            lane_b.push_back(agents.spawn(merging, 326.0f - static_cast<float>(i) * 9, car()));
        }
        const float lane_end = net.track_length(ends);
        float closest = 1e9f, furthest_on_ending = 0;
        int arrived = 0, ticks = 0, merged_in_turn = 0;
        bool last_was_merger = false, first = true;
        for (; ticks < 60 * 400 && arrived < 2 * kEach; ++ticks) {
            agents.tick(net, kDt);
            if (ticks % 3 == 0) closest = std::min(closest, closest_pair(agents, net));
            for (u32 id : lane_b) {
                if (!agents.valid(id)) continue;
                const RouteStep* step = agents.current_step(id);
                if (step && step->kind == RouteStep::Kind::Track && step->index == net.track_index(ends))
                    furthest_on_ending = std::max(furthest_on_ending, agents.agent(id).distance);
            }
            // Who arrives after whom: a merger behind a through car, or the
            // other way round, is one more turn taken.
            for (u32 i = 0; i < agents.agents().size(); ++i) {
                if (!agents.valid(i) || !agents.agent(i).done) continue;
                const bool merger = std::find(lane_b.begin(), lane_b.end(), i) != lane_b.end();
                if (!first && merger != last_was_merger) ++merged_in_turn;
                last_was_merger = merger;
                first = false;
            }
            arrived += clear_arrived(agents);
        }
        CHECK(arrived == 2 * kEach);
        CHECK(furthest_on_ending <= lane_end + 1e-3f); // nobody drove off the end of the lane
        CHECK(closest > 3.0f);
        // Not one lane emptying and then the other: they went through mixed.
        CHECK(merged_in_turn >= 4);
    }
}

void test_determinism() {
    u32 e1 = 0, e2 = 0;
    const CurveNetwork net = make_two_lanes(e1, e2);
    const auto make = [&] {
        NetworkAgents agents;
        const Route across = net.route(TrackRef{e1, 0}, TrackRef{e2, 1});
        const Route back = net.route(TrackRef{e1, 1}, TrackRef{e2, 0});
        for (int i = 0; i < 6; ++i) {
            NetAgentParams p = car();
            p.max_speed = 10.0f + static_cast<float>(i % 3) * 2;
            agents.spawn(i % 2 ? across : back, 200.0f - static_cast<float>(i) * 22, p);
        }
        return agents;
    };
    NetworkAgents a = make(), b = make();
    float closest = 1e9f;
    for (int t = 0; t < 60 * 60; ++t) {
        a.tick(net, kDt);
        b.tick(net, kDt);
        if (t % 4 == 0) closest = std::min(closest, closest_pair(a, net));
    }
    bool same = true;
    for (u32 i = 0; i < a.agents().size(); ++i) {
        const NetAgent& p = a.agent(i);
        const NetAgent& q = b.agent(i);
        same = same && p.distance == q.distance && p.speed == q.speed && p.step == q.step && p.accel == q.accel;
    }
    CHECK(same);
    CHECK(closest > 3.0f); // six cars swapping lanes, and never two in one place
}

} // namespace

int main() {
    test_free_road_and_stopping();
    test_queue_and_wave();
    test_models_side_by_side();
    test_change_along_the_track();
    test_pocket_and_lane_drop();
    test_determinism();
    if (failures == 0) std::printf("flow_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
