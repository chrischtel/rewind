// Headless checks for traffic lights as the road builder sets them up:
// presets, programs by time of day, editing, and repair after the roads change.

#include "../game/roads/build.h"
#include "../game/roads/signals.h"

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

CurveNetwork make_net() {
    CurveNetwork net;
    net.set_connect_filter(may_connect);
    net.set_track_trim(arm_trim);
    return net;
}

void build(CurveNetwork& net, const Path& path, u32 type = 0) {
    CHECK(lay_road(net, path, type).ok);
    refresh(net);
}

/// A plain crossing of two streets, and the node in the middle.
u32 crossing_of(CurveNetwork& net) {
    build(net, straight({-80, 0}, {80, 0}));
    build(net, straight({0, -60}, {0, 60}));
    return net.nearest_node({0, 0}, 3);
}

const auto no_demand = [](const SignalBox&, const SignalPhase&) { return false; };

void run(Signals& s, float seconds, float minute = 600) {
    for (int i = 0; i < static_cast<int>(seconds * 10 + 0.5f); ++i) tick_signals(s, 0.1f, minute, no_demand);
}

void test_presets() {
    CurveNetwork net = make_net();
    const u32 node = crossing_of(net);
    Signals signals;
    CHECK(!add_lights(net, signals, net.edge(0).a == node ? net.edge(0).b : net.edge(0).a)); // a road end: no lights
    SignalBox* box = add_lights(net, signals, node, kEachRoad);
    CHECK(box && box->arms.size() == 4 && box->moves.size() == 12);

    // Each road alone: four phases, nothing crossing in any of them, nothing left out.
    CHECK(box->plan().phases.size() == 4);
    for (const SignalPhase& p : box->plan().phases) CHECK(phase_clash(net, *box, p) == Clash::None && p.groups.size() == 3);
    CHECK(moves_left_out(*box, box->plan()) == 0);

    // Facing roads together: two phases, and the left turns in them are permitted, not protected.
    apply_preset(net, *box, kOpposite);
    CHECK(box->plan().phases.size() == 2);
    for (const SignalPhase& p : box->plan().phases) CHECK(phase_clash(net, *box, p) == Clash::Permitted && p.groups.size() == 6);
    CHECK(moves_left_out(*box, box->plan()) == 0);

    // Left turns on their own: four phases, none of them with anything crossing.
    apply_preset(net, *box, kProtected);
    CHECK(box->plan().phases.size() == 4);
    for (const SignalPhase& p : box->plan().phases) CHECK(phase_clash(net, *box, p) == Clash::None);
    CHECK(moves_left_out(*box, box->plan()) == 0);
    CHECK(box->plan().phases[1].green < box->plan().phases[0].green);

    // Two roads going straight across each other in one phase is reported.
    SignalPhase bad;
    for (u32 g = 1; g <= box->moves.size(); ++g)
        if (turn_of_move(net, node, box->moves[g - 1]) == 0) bad.groups.push_back(g);
    CHECK(bad.groups.size() == 4 && phase_clash(net, *box, bad) == Clash::Unsafe);

    // A T has one road nothing faces: it gets a phase to itself.
    CurveNetwork tee = make_net();
    build(tee, straight({-80, 0}, {80, 0}));
    build(tee, straight({0, 60}, {0, 0}));
    Signals ts;
    SignalBox* tb = add_lights(tee, ts, tee.nearest_node({0, 0}, 3), kOpposite);
    CHECK(tb && tb->moves.size() == 6 && tb->plan().phases.size() == 2);
    CHECK(moves_left_out(*tb, tb->plan()) == 0);
}

void test_running() {
    CurveNetwork net = make_net();
    const u32 node = crossing_of(net);
    Signals signals;
    SignalBox* box = add_lights(net, signals, node, kOpposite);
    const Move first = box->moves[box->plan().phases[0].groups[0] - 1];
    const Move second = box->moves[box->plan().phases[1].groups[0] - 1];
    CHECK(aspect_of(*box, first.from, first.to) == Aspect::Green);
    CHECK(aspect_of(*box, second.from, second.to) == Aspect::Red);
    run(signals, 21);
    CHECK(aspect_of(*box, first.from, first.to) == Aspect::Amber);
    run(signals, 3.5f); // all red, and the next road shows red-amber before its green
    CHECK(aspect_of(*box, first.from, first.to) == Aspect::Red);
    CHECK(aspect_of(*box, second.from, second.to) == Aspect::RedAmber);
    run(signals, 1.5f);
    CHECK(aspect_of(*box, second.from, second.to) == Aspect::Green);
    CHECK(aspect_of(*box, 9999, 9999) == Aspect::Red); // no such movement

    // The pedestrians get a phase in which every movement is red.
    set_walk(*box, true);
    CHECK(box->plan().phases.size() == 3 && !walking(*box));
    run(signals, 2 * 25 + 1);
    CHECK(walking(*box));
    for (const Move& m : box->moves) CHECK(aspect_of(*box, m.from, m.to) == Aspect::Red);
    set_walk(*box, false);
    CHECK(box->plan().phases.size() == 2);

    // An adaptive green lasts while cars keep coming, up to its limit.
    box->programs[0].adaptive = true;
    box->state = SignalState{};
    const auto busy = [](const SignalBox&, const SignalPhase&) { return true; };
    for (int i = 0; i < 300; ++i) tick_signals(signals, 0.1f, 600, busy);
    CHECK(box->state.phase == 0 && aspect_of(*box, first.from, first.to) == Aspect::Green); // 30 s in, still green
    for (int i = 0; i < 160; ++i) tick_signals(signals, 0.1f, 600, busy);
    CHECK(box->state.phase == 1); // 40 s was the limit
}

void test_programs() {
    CurveNetwork net = make_net();
    const u32 node = crossing_of(net);
    Signals signals;
    SignalBox* box = add_lights(net, signals, node, kOpposite);
    // A morning program from 06:00 with a long green for the first road.
    const u32 morning = add_program(*box, 0, 6);
    CHECK(morning == 1 && box->programs[1].start == 360);
    change_green(*box, morning, 0, 25);
    CHECK(box->programs[1].plan.phases[0].green == 45 && box->programs[0].plan.phases[0].green == 20);
    shift_start(*box, 0, 10 * 60); // the day program from 10:00
    CHECK(box->programs[0].start == 600);

    CHECK(program_at(*box, 7 * 60) == 1);
    CHECK(program_at(*box, 12 * 60) == 0);
    CHECK(program_at(*box, 3 * 60) == 0); // before the first start of the day: still yesterday's last

    // The change waits for the end of the phase that is running.
    run(signals, 5, 12 * 60);
    CHECK(box->running == 0);
    run(signals, 5, 7 * 60);
    CHECK(box->running == 0 && box->state.phase == 0);
    run(signals, 21, 7 * 60);
    CHECK(box->running == 1 && box->state.phase == 0);

    for (u32 i = 0; i < 6; ++i) add_program(*box, 0);
    CHECK(box->programs.size() == kMostPrograms);
    remove_program(*box, 3);
    remove_program(*box, 2);
    remove_program(*box, 1);
    remove_program(*box, 0); // the last one stays
    CHECK(box->programs.size() == 1);
}

void test_editing_and_repair() {
    CurveNetwork net = make_net();
    const u32 node = crossing_of(net);
    Signals signals;
    SignalBox* box = add_lights(net, signals, node, kEachRoad);
    const u32 fresh = add_phase(*box, 0, 0);
    CHECK(fresh == 1 && box->plan().phases.size() == 5 && box->plan().phases[1].groups.empty() && box->preset == kCustom);
    toggle_move(*box, 0, fresh, 2);
    toggle_move(*box, 0, fresh, 5);
    CHECK(box->plan().phases[1].groups == (std::vector<u32>{2, 5}));
    toggle_move(*box, 0, fresh, 2);
    CHECK(box->plan().phases[1].groups == (std::vector<u32>{5}));
    toggle_move(*box, 0, fresh, 99); // no such movement
    CHECK(box->plan().phases[1].groups.size() == 1);
    remove_phase(*box, 0, fresh);
    CHECK(box->plan().phases.size() == 4);
    change_green(*box, 0, 0, -100);
    CHECK(box->plan().phases[0].green == kLeastGreen);
    for (u32 i = 0; i < 20; ++i) add_phase(*box, 0, 0);
    CHECK(box->plan().phases.size() == kMostPhases);

    // Nothing changed about the roads: nothing to repair.
    CHECK(!repair(net, signals));
    // Another road into the junction: the plans are written again for five roads.
    build(net, straight({0, 0}, {60, 60}));
    CHECK(repair(net, signals));
    box = signals.at(node);
    CHECK(box && box->arms.size() == 5 && moves_left_out(*box, box->plan()) == 0);
    // The junction goes: so do its lights.
    net.clear();
    CHECK(repair(net, signals) && signals.boxes.empty());
}

} // namespace

int main() {
    test_presets();
    test_running();
    test_programs();
    test_editing_and_repair();
    if (failures == 0) std::printf("signals_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
