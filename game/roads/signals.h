#pragma once

// Traffic lights on a junction, as the player sets them up.
//
// Pure logic over the engine's CurveNetwork and its signal plans
// (rewind/sim/signal.h): no renderer, no cars, so it runs in a test without a
// window. Everything here is simulation state and reflects, so lights rewind
// and replay with the rest.
//
// A junction with lights has a SignalBox. The box knows the junction's
// movements (from one road into another: the unit a light is given to), and
// one or more programs. A program is a signal plan, a cycle of phases, plus
// the time of day it starts to run from; the box runs whichever program the
// clock says. That is how the morning gets a long green out of the motorway
// exit and the night a short cycle: two programs, two start times.
//
// A phase may let movements run together that cross each other, as long as
// one of each such pair is a turn: the turn is then "permitted", it has green
// but gives way, like a left turn that waits for a gap in oncoming traffic.
// Two movements going straight across each other in one phase is a mistake
// and is reported as one.

#include "lanes.h"

#include <rewind/sim/signal.h>

#include <algorithm>
#include <vector>

namespace roads {

using rw::Aspect;
using rw::SignalPhase;
using rw::SignalPlan;
using rw::SignalState;

/// One movement through a junction: from this road into that one.
struct Move {
    u32 from = 0;
    u32 to = 0;
    bool operator==(const Move&) const = default;
    void reflect(rw::Visitor& v) {
        v.field("from", from);
        v.field("to", to);
    }
};

/// Ready-made ways to set a junction up. The player starts from one and
/// changes what they like, which makes it Custom.
enum Preset : u32 {
    kEachRoad,  ///< every road gets its own green: simple, safe, slow
    kOpposite,  ///< roads facing each other go together; left turns wait for a gap
    kProtected, ///< facing roads go together, and their left turns get a phase of their own
    kCustom,
    kPresetCount
};

/// A cycle of phases and the time of day from which it runs.
struct Program {
    u32 start = 0;         ///< minutes after midnight
    SignalPlan plan;
    bool adaptive = false; ///< greens stretch while cars keep coming, up to each phase's max_green

    void reflect(rw::Visitor& v) {
        v.field("start", start, {.min = 0, .max = 1439});
        v.field("plan", plan);
        v.field("adaptive", adaptive);
    }
};

constexpr u32 kMostPrograms = 4;
constexpr u32 kMostPhases = 8;
constexpr float kLeastGreen = 4, kMostGreen = 120;

struct SignalBox {
    u32 node = 0;
    std::vector<u32> arms;   ///< the roads here when the box was set up: if they change, it is set up again
    std::vector<Move> moves; ///< a movement's group in the plans is its place here + 1
    std::vector<Program> programs;
    u32 preset = kOpposite;
    bool walk = false;       ///< a phase in which everything is red and people cross
    u32 running = 0;         ///< the program in force
    SignalState state;

    /// The group of the pedestrians' phase: one past the last movement.
    u32 walk_group() const { return static_cast<u32>(moves.size()) + 1; }
    u32 group_of(u32 from, u32 to) const {
        for (u32 i = 0; i < moves.size(); ++i)
            if (moves[i].from == from && moves[i].to == to) return i + 1;
        return 0;
    }
    const SignalPlan& plan() const { return programs[std::min<usize>(running, programs.size() - 1)].plan; }

    void reflect(rw::Visitor& v) {
        v.field("node", node);
        v.field("arms", arms);
        v.field("moves", moves);
        v.field("programs", programs);
        v.field("preset", preset);
        v.field("walk", walk);
        v.field("running", running);
        v.field("state", state);
    }
};

struct Signals {
    std::vector<SignalBox> boxes;

    SignalBox* at(u32 node) {
        for (SignalBox& b : boxes)
            if (b.node == node) return &b;
        return nullptr;
    }
    const SignalBox* at(u32 node) const { return const_cast<Signals*>(this)->at(node); }
    bool walk_at(u32 node) const {
        const SignalBox* b = at(node);
        return b && b->walk;
    }
    void reflect(rw::Visitor& v) { v.field("boxes", boxes); }
};

// ---- what a junction's movements are ------------------------------------------------------------

inline std::vector<Move> moves_at(const CurveNetwork& n, u32 node) {
    std::vector<Move> out;
    for (u32 c : n.connectors_at(node)) {
        const rw::Connector& k = n.connectors()[c];
        const Move m{k.from.edge, k.to.edge};
        if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
    }
    std::sort(out.begin(), out.end(), [](const Move& a, const Move& b) { return a.from != b.from ? a.from < b.from : a.to < b.to; });
    return out;
}

/// Which way a movement turns: as turn_of, from any one of its lanes.
inline int turn_of_move(const CurveNetwork& n, u32 node, const Move& m) {
    for (u32 c : n.connectors_at(node)) {
        const rw::Connector& k = n.connectors()[c];
        if (k.from.edge == m.from && k.to.edge == m.to) return turn_of(n, k.from, k.to);
    }
    return kBack;
}

/// Do two movements cross each other anywhere?
inline bool moves_cross(const CurveNetwork& n, u32 node, const Move& a, const Move& b) {
    if (a == b) return false;
    for (u32 c : n.connectors_at(node)) {
        const rw::Connector& k = n.connectors()[c];
        if (k.from.edge != a.from || k.to.edge != a.to) continue;
        for (u32 i : n.conflicts_of(c)) {
            const rw::Conflict& x = n.conflicts()[i];
            if (x.kind != rw::ConflictKind::Crossing) continue;
            const rw::Connector& o = n.connectors()[x.connector_a == c ? x.connector_b : x.connector_a];
            if (o.from.edge == b.from && o.to.edge == b.to) return true;
        }
    }
    return false;
}

/// What is wrong with letting the movements of one phase run together.
enum class Clash : rw::u8 {
    None,      ///< nothing crosses
    Permitted, ///< something crosses, but one of each pair is a turn and gives way
    Unsafe,    ///< two movements going straight on cross each other
};

inline Clash phase_clash(const CurveNetwork& n, const SignalBox& box, const SignalPhase& phase) {
    Clash worst = Clash::None;
    for (usize i = 0; i < phase.groups.size(); ++i)
        for (usize j = i + 1; j < phase.groups.size(); ++j) {
            const u32 a = phase.groups[i], b = phase.groups[j];
            if (a == 0 || b == 0 || a > box.moves.size() || b > box.moves.size()) continue;
            if (!moves_cross(n, box.node, box.moves[a - 1], box.moves[b - 1])) continue;
            const bool both_straight = turn_of_move(n, box.node, box.moves[a - 1]) == 0 && turn_of_move(n, box.node, box.moves[b - 1]) == 0;
            if (both_straight) return Clash::Unsafe;
            worst = Clash::Permitted;
        }
    return worst;
}

/// A movement that is in no phase of a plan never gets green: the number of those.
inline u32 moves_left_out(const SignalBox& box, const SignalPlan& plan) {
    u32 count = 0;
    for (u32 g = 1; g <= box.moves.size(); ++g) {
        bool found = false;
        for (const SignalPhase& p : plan.phases) found = found || p.has(g);
        count += found ? 0 : 1;
    }
    return count;
}

// ---- ready-made plans ---------------------------------------------------------------------------------

/// The road facing this one across the junction: the one its traffic carries
/// straight on into. ~0u when there is none.
inline u32 facing_arm(const CurveNetwork& n, u32 node, u32 arm) {
    for (u32 c : n.connectors_at(node)) {
        const rw::Connector& k = n.connectors()[c];
        if (turn_of(n, k.from, k.to) != 0) continue;
        if (k.from.edge == arm) return k.to.edge;
        if (k.to.edge == arm) return k.from.edge;
    }
    return ~0u;
}

/// A plan for a box after one of the presets. `green` is the main green in
/// seconds; left-turn phases get a little over half of it.
inline SignalPlan preset_plan(const CurveNetwork& n, const SignalBox& box, u32 preset, float green = 20) {
    SignalPlan plan;
    const auto phase_of = [&](float seconds, auto&& wanted) {
        SignalPhase p;
        p.green = seconds;
        p.max_green = seconds * 2;
        for (u32 i = 0; i < box.moves.size(); ++i)
            if (wanted(box.moves[i])) p.groups.push_back(i + 1);
        if (!p.groups.empty()) plan.phases.push_back(std::move(p));
    };
    // The roads in pairs that face each other; a road nothing faces is alone.
    std::vector<std::pair<u32, u32>> pairs;
    std::vector<u32> seen;
    for (u32 arm : box.arms) {
        if (std::find(seen.begin(), seen.end(), arm) != seen.end()) continue;
        u32 other = preset == kEachRoad ? ~0u : facing_arm(n, box.node, arm);
        if (other != ~0u && std::find(seen.begin(), seen.end(), other) != seen.end()) other = ~0u;
        pairs.emplace_back(arm, other);
        seen.push_back(arm);
        if (other != ~0u) seen.push_back(other);
    }
    for (const auto& [a, b] : pairs) {
        const auto from_pair = [&, a = a, b = b](const Move& m) { return m.from == a || m.from == b; };
        bool lefts = false;
        for (const Move& m : box.moves) lefts = lefts || (from_pair(m) && turn_of_move(n, box.node, m) == -1);
        if (preset == kProtected && b != ~0u && lefts) {
            phase_of(green, [&](const Move& m) { return from_pair(m) && turn_of_move(n, box.node, m) != -1; });
            phase_of(std::max(8.0f, std::round(green * 0.6f)), [&](const Move& m) { return from_pair(m) && turn_of_move(n, box.node, m) == -1; });
        } else {
            phase_of(green, from_pair);
        }
    }
    if (box.walk) {
        SignalPhase p;
        p.green = 10;
        p.groups.push_back(box.walk_group());
        plan.phases.push_back(std::move(p));
    }
    return plan;
}

// ---- setting a junction up, and keeping it right when the roads change -------------------------------

inline std::vector<u32> arms_now(const CurveNetwork& n, u32 node) {
    std::vector<u32> arms = n.edges_at(node);
    std::sort(arms.begin(), arms.end());
    return arms;
}

inline bool can_have_lights(const CurveNetwork& n, u32 node) { return n.valid_node(node) && n.edges_at(node).size() >= 3 && !n.connectors_at(node).empty(); }

/// Set every program of a box up afresh from a preset, keeping how many
/// programs there are and when they start.
inline void apply_preset(const CurveNetwork& n, SignalBox& box, u32 preset) {
    box.arms = arms_now(n, box.node);
    box.moves = moves_at(n, box.node);
    box.preset = preset < kCustom ? preset : kOpposite;
    if (box.programs.empty()) box.programs.push_back(Program{});
    for (Program& p : box.programs) {
        const float green = p.plan.phases.empty() ? 20.0f : p.plan.phases.front().green;
        const SignalPlan fresh = preset_plan(n, box, box.preset, green);
        p.plan.phases = fresh.phases;
    }
    box.running = std::min<u32>(box.running, static_cast<u32>(box.programs.size()) - 1);
    box.state = SignalState{};
}

/// Put lights on a junction. Returns the box, or nullptr when it cannot have any.
inline SignalBox* add_lights(const CurveNetwork& n, Signals& signals, u32 node, u32 preset = kOpposite) {
    if (!can_have_lights(n, node)) return nullptr;
    SignalBox* box = signals.at(node);
    if (!box) {
        signals.boxes.push_back(SignalBox{});
        box = &signals.boxes.back();
        box->node = node;
    }
    apply_preset(n, *box, preset);
    return box;
}

inline void remove_lights(Signals& signals, u32 node) {
    std::erase_if(signals.boxes, [&](const SignalBox& b) { return b.node == node; });
}

/// After the network changed: a junction that is gone loses its lights, and
/// one whose roads are no longer the ones its plans were written for is set
/// up again from its preset. Returns whether anything changed.
inline bool repair(const CurveNetwork& n, Signals& signals) {
    bool changed = false;
    const usize before = signals.boxes.size();
    std::erase_if(signals.boxes, [&](const SignalBox& b) { return !can_have_lights(n, b.node); });
    changed = signals.boxes.size() != before;
    for (SignalBox& box : signals.boxes) {
        if (box.arms == arms_now(n, box.node) && box.moves == moves_at(n, box.node) && !box.programs.empty()) continue;
        apply_preset(n, box, box.preset);
        changed = true;
    }
    return changed;
}

// ---- editing: what the buttons do ------------------------------------------------------------------

inline Program* program_of(SignalBox& box, u32 program) { return program < box.programs.size() ? &box.programs[program] : nullptr; }
inline SignalPhase* phase_of(SignalBox& box, u32 program, u32 phase) {
    Program* p = program_of(box, program);
    return p && phase < p->plan.phases.size() ? &p->plan.phases[phase] : nullptr;
}

/// Put a movement into a phase, or take it out.
inline void toggle_move(SignalBox& box, u32 program, u32 phase, u32 group) {
    SignalPhase* p = phase_of(box, program, phase);
    if (!p || group == 0 || group > box.moves.size()) return;
    const auto it = std::find(p->groups.begin(), p->groups.end(), group);
    if (it != p->groups.end()) p->groups.erase(it);
    else p->groups.push_back(group);
    std::sort(p->groups.begin(), p->groups.end());
    box.preset = kCustom;
}

inline void change_green(SignalBox& box, u32 program, u32 phase, float by) {
    SignalPhase* p = phase_of(box, program, phase);
    if (!p) return;
    p->green = std::clamp(p->green + by, kLeastGreen, kMostGreen);
    p->max_green = std::max(p->max_green, p->green);
}
inline void change_max_green(SignalBox& box, u32 program, u32 phase, float by) {
    SignalPhase* p = phase_of(box, program, phase);
    if (!p) return;
    p->max_green = std::clamp(p->max_green + by, p->green, kMostGreen * 2);
}

/// A new, empty phase after `phase`. Returns its place, or `phase` when the plan is full.
inline u32 add_phase(SignalBox& box, u32 program, u32 phase) {
    Program* p = program_of(box, program);
    if (!p || p->plan.phases.size() >= kMostPhases) return phase;
    SignalPhase fresh;
    fresh.green = 10;
    fresh.max_green = 20;
    const u32 at = std::min<u32>(phase + 1, static_cast<u32>(p->plan.phases.size()));
    p->plan.phases.insert(p->plan.phases.begin() + at, fresh);
    box.preset = kCustom;
    box.state = SignalState{};
    return at;
}
inline void remove_phase(SignalBox& box, u32 program, u32 phase) {
    Program* p = program_of(box, program);
    if (!p || phase >= p->plan.phases.size() || p->plan.phases.size() <= 1) return;
    p->plan.phases.erase(p->plan.phases.begin() + phase);
    box.preset = kCustom;
    box.state = SignalState{};
}

/// Turn the pedestrians' phase on or off, in every program.
inline void set_walk(SignalBox& box, bool on) {
    box.walk = on;
    const u32 g = box.walk_group();
    for (Program& p : box.programs) {
        std::erase_if(p.plan.phases, [&](const SignalPhase& ph) { return ph.groups.size() == 1 && ph.groups[0] == g; });
        if (on && p.plan.phases.size() < kMostPhases) {
            SignalPhase walk;
            walk.green = 10;
            walk.groups.push_back(g);
            p.plan.phases.push_back(walk);
        }
        if (p.plan.phases.empty()) p.plan.phases.push_back(SignalPhase{});
    }
    box.state = SignalState{};
}

/// Another program: a copy of `from`, starting `hours` later. Returns its place.
inline u32 add_program(SignalBox& box, u32 from, u32 hours = 3) {
    if (box.programs.size() >= kMostPrograms || from >= box.programs.size()) return from;
    Program copy = box.programs[from];
    copy.start = (copy.start + hours * 60) % 1440;
    box.programs.push_back(copy);
    return static_cast<u32>(box.programs.size()) - 1;
}
inline void remove_program(SignalBox& box, u32 program) {
    if (box.programs.size() <= 1 || program >= box.programs.size()) return;
    box.programs.erase(box.programs.begin() + program);
    box.running = 0;
    box.state = SignalState{};
}
inline void shift_start(SignalBox& box, u32 program, int minutes) {
    if (Program* p = program_of(box, program)) p->start = static_cast<u32>(((static_cast<int>(p->start) + minutes) % 1440 + 1440) % 1440);
}

// ---- running --------------------------------------------------------------------------------------

/// The program in force at a time of day: the one that started most
/// recently, which before the first start of the day is the last of the day
/// before.
inline u32 program_at(const SignalBox& box, float minute_of_day) {
    u32 best = 0;
    int best_ago = 1 << 30;
    for (u32 i = 0; i < box.programs.size(); ++i) {
        int ago = static_cast<int>(minute_of_day) - static_cast<int>(box.programs[i].start);
        if (ago < 0) ago += 1440;
        if (ago < best_ago) {
            best_ago = ago;
            best = i;
        }
    }
    return best;
}

/// One step for every box. `demand(box, phase)` says whether cars are still
/// arriving for a phase: an adaptive program holds its green while they are.
/// A change of program waits for the end of the phase that is running, so
/// nobody loses a green half way.
template <typename Demand>
inline void tick_signals(Signals& signals, float dt, float minute_of_day, Demand&& demand) {
    for (SignalBox& box : signals.boxes) {
        if (box.programs.empty()) continue;
        box.running = std::min<u32>(box.running, static_cast<u32>(box.programs.size()) - 1);
        const Program& program = box.programs[box.running];
        if (program.plan.phases.empty()) continue;
        if (box.state.phase >= program.plan.phases.size()) box.state = SignalState{};
        const u32 before = box.state.phase;
        const bool wanted = program.adaptive && demand(box, program.plan.phases[before]);
        rw::signal_tick(program.plan, box.state, dt, wanted);
        const u32 due = program_at(box, minute_of_day);
        if (due != box.running && (box.state.phase != before || program.plan.phases.size() == 1)) {
            box.running = due;
            box.state = SignalState{};
        }
    }
}

/// What a movement is shown right now.
inline Aspect aspect_of(const SignalBox& box, u32 from, u32 to) {
    const u32 group = box.group_of(from, to);
    if (group == 0 || box.programs.empty()) return Aspect::Red;
    return rw::signal_aspect(box.plan(), box.state, group);
}

/// Are people crossing right now?
inline bool walking(const SignalBox& box) {
    return box.walk && !box.programs.empty() && rw::signal_aspect(box.plan(), box.state, box.walk_group()) == Aspect::Green;
}

} // namespace roads
