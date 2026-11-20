#pragma once

// Signals: a plan of phases, the state of a controller running it, and what
// each group is showing right now. Road traffic lights, railway signals at a
// crossing, a lock on a canal: anything where groups of movements take turns.
//
// A plan is authored data and a state is simulation state; both reflect, so
// a signal rewinds and replays with everything else. The functions are pure:
// the same plan, state and dt always give the same next state.
//
// A group is just a number. On a CurveNetwork it is the `group` of an
// authored turn (see sim/network.h), and the helpers at the bottom derive
// groups and a first plan from a node's conflicts, which is what "put
// lights on this junction" does before the player starts editing.

#include "rewind/core/types.h"
#include "rewind/sim/network.h"
#include "rewind/sim/reflect.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rw {

enum class Aspect : u8 {
    Red,
    RedAmber, ///< about to turn green (shown in much of Europe); treat as red
    Green,
    Amber,    ///< green has ended: stop if you can
};

/// One step of the cycle: the groups that run together, and for how long.
struct SignalPhase {
    std::vector<u32> groups;
    float green = 20;    ///< seconds of green; the minimum when the phase is actuated
    float max_green = 0; ///< > green: the phase may hold its green this long while there is demand

    bool has(u32 group) const { return std::find(groups.begin(), groups.end(), group) != groups.end(); }

    void reflect(Visitor& v) {
        v.field("groups", groups);
        v.field("green", green, {.min = 1, .max = 300});
        v.field("max_green", max_green, {.min = 0, .max = 600});
    }
};

/// The phases in order and the change-over between them. After a phase's
/// green come `amber` and then `all_red` seconds in which nothing new starts;
/// the groups of the next phase show red-amber for the last `red_amber`
/// seconds of that. A group that is in two phases running one after the other
/// keeps its green across the change-over.
struct SignalPlan {
    std::vector<SignalPhase> phases;
    float amber = 3;
    float all_red = 2;
    float red_amber = 1; ///< 0 = straight from red to green
    float offset = 0;    ///< seconds this plan runs behind the clock: line junctions up for a green wave

    float change_over() const { return amber + all_red; }
    /// Length of one whole cycle without any extension.
    float cycle() const {
        float total = 0;
        for (const SignalPhase& p : phases) total += p.green + change_over();
        return total;
    }

    void reflect(Visitor& v) {
        v.field("phases", phases);
        v.field("amber", amber, {.min = 0, .max = 10});
        v.field("all_red", all_red, {.min = 0, .max = 30});
        v.field("red_amber", red_amber, {.min = 0, .max = 5});
        v.field("offset", offset);
    }
};

/// Where a controller is in its plan.
struct SignalState {
    u32 phase = 0;
    float time = 0;     ///< seconds since this phase's green began
    float extended = 0; ///< extra green granted to this phase so far

    void reflect(Visitor& v) {
        v.field("phase", phase);
        v.field("time", time);
        v.field("extended", extended);
    }
};

/// Seconds of green the current phase gets, extension included.
inline float signal_green(const SignalPlan& plan, const SignalState& state) {
    if (state.phase >= plan.phases.size()) return 0;
    return plan.phases[state.phase].green + state.extended;
}

/// Advance a controller. `demand` is whether something still wants the
/// current green: an actuated phase holds its green while that is true, up to
/// its max_green, and a fixed phase ignores it.
inline void signal_tick(const SignalPlan& plan, SignalState& state, float dt, bool demand = false) {
    if (plan.phases.empty()) return;
    if (state.phase >= plan.phases.size()) state = SignalState{};
    const SignalPhase& phase = plan.phases[state.phase];
    const float green = phase.green + state.extended;
    // Still in green and about to run out: an actuated phase with demand
    // pushes its end to just past this step, so the green lasts as long as
    // the demand does and ends within a step and a half of it stopping. The
    // half step of slack keeps rounding from ending it early.
    if (demand && phase.max_green > phase.green && state.time < green && state.time + dt >= green) {
        const float wanted = state.time + dt * 1.5f - phase.green;
        state.extended = std::min(std::max(state.extended, wanted), phase.max_green - phase.green);
    }
    state.time += dt;
    const float slot = phase.green + state.extended + plan.change_over();
    if (state.time >= slot) {
        state.time -= slot;
        state.phase = (state.phase + 1) % static_cast<u32>(plan.phases.size());
        state.extended = 0;
    }
}

/// The state a fixed-time plan is in at `clock` seconds, offset included.
/// Two junctions whose plans share a cycle stay in step for ever this way,
/// whenever each was built.
inline SignalState signal_state_at(const SignalPlan& plan, float clock) {
    SignalState state;
    const float cycle = plan.cycle();
    if (plan.phases.empty() || cycle <= 0) return state;
    float t = std::fmod(clock - plan.offset, cycle);
    if (t < 0) t += cycle;
    for (u32 i = 0; i < plan.phases.size(); ++i) {
        const float slot = plan.phases[i].green + plan.change_over();
        if (t < slot || i + 1 == plan.phases.size()) {
            state.phase = i;
            state.time = t;
            return state;
        }
        t -= slot;
    }
    return state;
}

/// What a group shows right now. A group in no phase at all is red.
inline Aspect signal_aspect(const SignalPlan& plan, const SignalState& state, u32 group) {
    if (state.phase >= plan.phases.size()) return Aspect::Red;
    const SignalPhase& phase = plan.phases[state.phase];
    const SignalPhase& next = plan.phases[(state.phase + 1) % plan.phases.size()];
    const float green = phase.green + state.extended;
    const bool now = phase.has(group);
    const bool then = next.has(group) && plan.phases.size() > 1;
    if (state.time < green) return now ? Aspect::Green : Aspect::Red;
    if (now && then) return Aspect::Green; // runs in both: no reason to stop it in between
    if (now) return state.time < green + plan.amber ? Aspect::Amber : Aspect::Red;
    if (then) {
        const float lead = std::min(plan.red_amber, plan.change_over());
        return lead > 0 && state.time >= green + plan.change_over() - lead ? Aspect::RedAmber : Aspect::Red;
    }
    return Aspect::Red;
}

inline bool signal_is_go(Aspect a) { return a == Aspect::Green; }

/// Seconds until a group next shows green: 0 while it is green, and a
/// negative number when it is in no phase. Extensions are not predicted.
inline float signal_time_to_green(const SignalPlan& plan, const SignalState& state, u32 group) {
    if (state.phase >= plan.phases.size()) return -1;
    if (signal_aspect(plan, state, group) == Aspect::Green) return 0;
    float wait = plan.phases[state.phase].green + state.extended + plan.change_over() - state.time;
    for (u32 step = 1; step <= plan.phases.size(); ++step) {
        const SignalPhase& p = plan.phases[(state.phase + step) % plan.phases.size()];
        if (p.has(group)) return wait;
        wait += p.green + plan.change_over();
    }
    return -1;
}

/// Seconds of green a group has left, 0 when it is not green. Counts on
/// through the next phase when the group runs in that too.
inline float signal_green_left(const SignalPlan& plan, const SignalState& state, u32 group) {
    if (signal_aspect(plan, state, group) != Aspect::Green) return 0;
    float left = plan.phases[state.phase].green + state.extended - state.time;
    for (u32 step = 1; step < plan.phases.size(); ++step) {
        const SignalPhase& p = plan.phases[(state.phase + step) % plan.phases.size()];
        if (!p.has(group)) break;
        left += plan.change_over() + p.green;
    }
    return std::max(left, 0.0f);
}

// ---- signals on a network ---------------------------------------------------------------------

/// How the turns at a node are put into groups.
enum class TurnGrouping : u8 {
    Approach, ///< one group per road arriving: everything from that road goes together
    Movement, ///< one group per pair of roads: the left turn from the north is its own group
    Lane,     ///< one group per arriving track
};

/// Author a node and give its turns groups, numbered from 1 in the order
/// roads and tracks are stored. Returns how many groups there are. Call
/// rebuild() afterwards, as after any edit.
inline u32 assign_turn_groups(CurveNetwork& net, u32 node, TurnGrouping grouping = TurnGrouping::Approach) {
    if (!net.valid_node(node)) return 0;
    net.author_node(node);
    struct Key {
        u32 a, b;
        bool operator==(const Key&) const = default;
    };
    std::vector<Key> keys;
    for (Turn& t : net.node(node).turns) {
        Key k{t.from.edge, 0};
        if (grouping == TurnGrouping::Movement) k.b = t.to.edge + 1;
        else if (grouping == TurnGrouping::Lane) k.b = t.from.track + 1;
        auto it = std::find(keys.begin(), keys.end(), k);
        if (it == keys.end()) {
            keys.push_back(k);
            it = keys.end() - 1;
        }
        t.group = static_cast<u32>(it - keys.begin()) + 1;
    }
    return static_cast<u32>(keys.size());
}

/// The pairs of groups at a node that must never be green together: those
/// with a connector each whose curves cross (or merge, when the network lists
/// merges). Each pair once, lower group first. Needs an up-to-date rebuild().
inline std::vector<std::pair<u32, u32>> conflicting_groups(const CurveNetwork& net, u32 node) {
    std::vector<std::pair<u32, u32>> out;
    for (u32 c : net.connectors_at(node))
        for (u32 index : net.conflicts_of(c)) {
            const Conflict& k = net.conflicts()[index];
            if (k.connector_a != c) continue; // each conflict from one side only
            u32 a = net.connectors()[k.connector_a].group, b = net.connectors()[k.connector_b].group;
            if (a == b) continue;
            if (a > b) std::swap(a, b);
            if (std::find(out.begin(), out.end(), std::make_pair(a, b)) == out.end()) out.emplace_back(a, b);
        }
    std::sort(out.begin(), out.end());
    return out;
}

/// The first phase of a plan in which two conflicting groups are green
/// together, or ~0u when the plan is safe. An editor shows this in red.
inline u32 signal_plan_clash(const SignalPlan& plan, const std::vector<std::pair<u32, u32>>& conflicts) {
    for (u32 i = 0; i < plan.phases.size(); ++i)
        for (const auto& [a, b] : conflicts)
            if (plan.phases[i].has(a) && plan.phases[i].has(b)) return i;
    return ~0u;
}

/// A first plan for a node: its groups packed into as few phases as their
/// conflicts allow, every group in the first phase that has room for it and
/// then in every later phase that has room too. Fixed time, `green` seconds
/// each. Groups are whatever the node's connectors carry; group 0 is left out.
inline SignalPlan signal_plan_for(const CurveNetwork& net, u32 node, float green = 20) {
    SignalPlan plan;
    std::vector<u32> groups;
    for (u32 c : net.connectors_at(node)) {
        const u32 g = net.connectors()[c].group;
        if (g != 0 && std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g);
    }
    std::sort(groups.begin(), groups.end());
    const std::vector<std::pair<u32, u32>> conflicts = conflicting_groups(net, node);
    const auto clash = [&](u32 a, u32 b) {
        const auto pair = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
        return std::find(conflicts.begin(), conflicts.end(), pair) != conflicts.end();
    };
    const auto fits = [&](const SignalPhase& phase, u32 g) {
        for (u32 other : phase.groups)
            if (clash(other, g)) return false;
        return true;
    };
    // First fit: each group opens a phase only when none so far can take it.
    for (u32 g : groups) {
        bool placed = false;
        for (SignalPhase& phase : plan.phases)
            if (fits(phase, g)) {
                phase.groups.push_back(g);
                placed = true;
                break;
            }
        if (!placed) {
            SignalPhase phase;
            phase.green = green;
            phase.groups.push_back(g);
            plan.phases.push_back(std::move(phase));
        }
    }
    // Then let every group ride along wherever else it does no harm: a right
    // turn that conflicts with nothing in another phase gets that green too.
    for (u32 g : groups)
        for (SignalPhase& phase : plan.phases)
            if (!phase.has(g) && fits(phase, g)) phase.groups.push_back(g);
    for (SignalPhase& phase : plan.phases) std::sort(phase.groups.begin(), phase.groups.end());
    return plan;
}

} // namespace rw
