#pragma once

// Agents moving along a CurveNetwork: each follows a planned Route, keeps
// its distance to whoever is ahead on the same piece (or the next one),
// changes tracks sideways over a transition, and waits before entering a
// connector that crosses one in use. Everything is reflected simulation
// state and updates in index order, so it rewinds and stays deterministic.
//
// How an agent picks its speed is its `model`. Simple accelerates and
// brakes at fixed rates and never runs faster than it can stop within the
// gap in front: exact, cheap, right for trains, belts and anything on
// rails. Idm is the Intelligent Driver Model: acceleration falls off
// smoothly towards the wanted speed and braking grows smoothly with how
// fast the gap is closing, so a queue eases to a stand and pulls away one
// after another, and stop-and-go waves appear on a busy road by themselves.
//
// A change of track happens along the track, not at its end: as soon as the
// track beside exists there and has a gap, the agent moves over while it
// keeps driving, occupying both tracks until it has arrived. One that finds
// no gap holds at the last place the change can still be made, and whoever
// comes up behind in the other track lets it in.
//
// Cars, trains, pedestrians, carts on a belt: the agents only know pieces
// (tracks, connectors, changes) and lengths. Right of way beyond "someone
// is already in the crossing" is the game's: give it an entry rule.

#include "rewind/core/types.h"
#include "rewind/sim/network.h"
#include "rewind/sim/reflect.h"

#include <functional>
#include <vector>

namespace rw {

enum class FollowModel : u8 {
    Simple, ///< fixed rates, never faster than it can stop in the gap ahead
    Idm,    ///< Intelligent Driver Model: smooth, and produces real queues and waves
};

struct NetAgentParams {
    float length = 4;        ///< space it occupies behind its front
    float max_speed = 100;   ///< the speed it wants on an open track
    float accel = 60;        ///< units per second squared; with Idm, the most it accelerates from rest
    float decel = 120;       ///< comfortable braking
    float min_gap = 2;       ///< standing distance to the one in front
    float headway = 0.6f;    ///< seconds of following distance at speed
    float change_length = 12; ///< how far a sideways track change stretches along the track
    FollowModel model = FollowModel::Simple;
    float exponent = 4;      ///< Idm: how late acceleration fades as the wanted speed nears (4 is the textbook value)
    float max_brake = 0;     ///< Idm: the hardest it ever brakes; 0 = three times `decel`

    void reflect(Visitor& v) {
        v.field("length", length, {.min = 0.1f});
        v.field("max_speed", max_speed, {.min = 0});
        v.field("accel", accel, {.min = 0});
        v.field("decel", decel, {.min = 0.1f});
        v.field("min_gap", min_gap, {.min = 0});
        v.field("headway", headway, {.min = 0});
        v.field("change_length", change_length, {.min = 0.1f});
        v.field("model", model);
        v.field("exponent", exponent, {.min = 1, .max = 8});
        v.field("max_brake", max_brake, {.min = 0});
    }
};

struct NetAgent {
    bool alive = false;
    Route route;
    usize step = 0;        ///< index into route.steps
    float distance = 0;    ///< of the front, along the current piece
    float speed = 0;
    float stop_distance = -1; ///< on the last track: stop here (< 0 = at its end)
    NetAgentParams params;
    u32 tag = 0;           ///< yours: an entity, a type, a colour
    bool waiting = false;  ///< held this tick: before a connector, or for a gap to change tracks
    bool done = false;
    float accel = 0;       ///< what its speed did this tick, per second: negative is braking (brake lights)
    float change_start = 0; ///< while changing tracks: where on the track it left the change began

    void reflect(Visitor& v) {
        v.field("alive", alive);
        v.field("route", route);
        v.field("step", step);
        v.field("distance", distance);
        v.field("speed", speed);
        v.field("stop_distance", stop_distance);
        v.field("params", params);
        v.field("tag", tag);
        v.field("waiting", waiting);
        v.field("done", done);
        v.field("accel", accel);
        v.field("change_start", change_start);
    }
};

/// What an agent may do this tick. The params are the vehicle's own limits;
/// a motion rule may hand back less (or more) once it has looked at where the
/// agent actually is.
struct MotionLimits {
    float accel = 0;
    float decel = 0;
    float max_speed = 0;
};

class NetworkAgents {
public:
    /// May this agent enter the connector now? Called when it reaches the
    /// end of its track; false holds it at the stop line. The default
    /// refuses while a conflicting connector has an agent on it. Signals
    /// and priority rules live in the game's version of this.
    using EntryRule = std::function<bool(const NetworkAgents&, u32 agent, u32 connector)>;
    void set_entry_rule(EntryRule rule) { rule_ = std::move(rule); }

    /// What may this agent do, here, now? Called once per agent per tick with
    /// its own params as the starting point; return them changed and the
    /// motion follows. This is where a game puts anything that makes the
    /// piece under the agent matter: a gradient it has to pull against, a
    /// speed limit, a load, a worn engine, a governor.
    ///
    /// A **negative** `accel` is legal and means the agent cannot hold its
    /// speed here at all -- a vehicle too heavy for the hill. It coasts down
    /// to a stand rather than misbehaving, and stays stopped until the rule
    /// gives it something to pull with again.
    ///
    /// Like the entry rule this is not reflected state: set it every run,
    /// and keep it a pure function of the simulation or rewind will lie.
    using MotionRule = std::function<MotionLimits(const NetworkAgents&, const CurveNetwork&, u32 agent,
                                                  MotionLimits from_params)>;
    void set_motion_rule(MotionRule rule) { motion_ = std::move(rule); }

    /// Add an agent at `distance` on the route's first track. Returns its index (reused after removal).
    u32 spawn(const Route& route, float distance = 0, const NetAgentParams& params = {}, u32 tag = 0);
    void remove(u32 agent);
    void clear();
    /// Replace the route of an agent that is on one of the new route's first-step track.
    bool reroute(u32 agent, const Route& route);

    /// Advance every agent by dt. `net` is the network the routes refer to.
    void tick(const CurveNetwork& net, float dt);

    const std::vector<NetAgent>& agents() const { return agents_; }
    NetAgent& agent(u32 i) { return agents_[i]; }
    const NetAgent& agent(u32 i) const { return agents_[i]; }
    bool valid(u32 i) const { return i < agents_.size() && agents_[i].alive; }
    usize count() const;

    /// Where an agent is and which way it faces.
    PathPoint pose(const CurveNetwork& net, u32 agent) const;
    /// The route step an agent is on.
    const RouteStep* current_step(u32 agent) const;
    /// Length of the piece an agent is on.
    float piece_length(const CurveNetwork& net, const NetAgent& a) const;
    /// Agents on a connector right now (for entry rules and drawing).
    std::vector<u32> on_connector(u32 connector) const;
    /// Agents on a track, front-most first.
    std::vector<u32> on_track(u32 track_index) const;
    /// Who an agent is following right now and how far ahead its tail is, as
    /// of the last tick. `agent` is ~0u on an open track.
    struct Lead {
        float gap = 1e9f;
        float speed = 0;
        u32 agent = ~0u;
    };
    Lead lead_of(const CurveNetwork& net, u32 agent) const;

    void reflect(Visitor& v) { v.field("agents", agents_); }

private:
    struct Piece {
        RouteStep::Kind kind;
        u32 index;
        bool operator==(const Piece&) const = default;
    };
    Piece piece_of(const NetAgent& a) const;
    float piece_len(const CurveNetwork& net, const NetAgent& a, const Piece& p) const;
    /// Something taking up room on a track that is not simply an agent on
    /// it: one that is half way across from the track beside, or one waiting
    /// beside for a gap.
    struct Slot {
        u32 agent;
        float front;  ///< on this track
        float length;
        float speed;
        bool soft;    ///< only those wholly behind it give way: it is not in the track yet
    };
    /// What is in front on the current piece and the following ones along the route (up to `lookahead`).
    Lead lead_ahead(const CurveNetwork& net, u32 agent, float lookahead) const;
    void scan_track(u32 track, float front, bool all_ahead, float base, u32 self, Lead& best) const;
    bool gap_beside(u32 track, float front, u32 self) const;
    bool try_change(const CurveNetwork& net, u32 agent);
    bool may_enter(const CurveNetwork& net, u32 agent, u32 connector) const;
    void advance(const CurveNetwork& net, u32 agent, float dt);

    MotionLimits limits_of(const CurveNetwork& net, u32 agent) const;

    std::vector<NetAgent> agents_;
    EntryRule rule_;
    MotionRule motion_;
    // Per-tick index: agents by piece, rebuilt at the start of tick().
    std::vector<std::vector<u32>> by_track_;
    std::vector<std::vector<u32>> by_connector_;
    std::vector<std::vector<u32>> by_change_;
    std::vector<std::vector<Slot>> slots_; ///< per track
};

} // namespace rw
