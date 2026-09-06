#pragma once

// Agents moving along a CurveNetwork: each follows a planned Route, keeps
// its distance to whoever is ahead on the same piece (or the next one),
// changes tracks sideways over a transition, and waits before entering a
// connector that crosses one in use. Speeds come from a simple
// accelerate-and-brake model: an agent never runs faster than it can stop
// within the gap in front. Everything is reflected simulation state and
// updates in index order, so it rewinds and stays deterministic.
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

struct NetAgentParams {
    float length = 4;        ///< space it occupies behind its front
    float max_speed = 100;
    float accel = 60;        ///< units per second squared
    float decel = 120;       ///< comfortable braking
    float min_gap = 2;       ///< standing distance to the one in front
    float headway = 0.6f;    ///< seconds of following distance at speed
    float change_length = 12; ///< how far a sideways track change stretches along the track

    void reflect(Visitor& v) {
        v.field("length", length, {.min = 0.1f});
        v.field("max_speed", max_speed, {.min = 0});
        v.field("accel", accel, {.min = 0});
        v.field("decel", decel, {.min = 0.1f});
        v.field("min_gap", min_gap, {.min = 0});
        v.field("headway", headway, {.min = 0});
        v.field("change_length", change_length, {.min = 0.1f});
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
    bool waiting = false;  ///< held before a connector this tick
    bool done = false;

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
    }
};

class NetworkAgents {
public:
    /// May this agent enter the connector now? Called when it reaches the
    /// end of its track; false holds it at the stop line. The default
    /// refuses while a conflicting connector has an agent on it. Signals
    /// and priority rules live in the game's version of this.
    using EntryRule = std::function<bool(const NetworkAgents&, u32 agent, u32 connector)>;
    void set_entry_rule(EntryRule rule) { rule_ = std::move(rule); }

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

    void reflect(Visitor& v) { v.field("agents", agents_); }

private:
    struct Piece {
        RouteStep::Kind kind;
        u32 index;
        bool operator==(const Piece&) const = default;
    };
    Piece piece_of(const NetAgent& a) const;
    float piece_len(const CurveNetwork& net, const NetAgent& a, const Piece& p) const;
    /// Gap in front on the current piece and the following ones along the route (up to `lookahead`).
    float gap_ahead(const CurveNetwork& net, u32 agent, float lookahead) const;
    bool may_enter(const CurveNetwork& net, u32 agent, u32 connector) const;
    void advance(const CurveNetwork& net, u32 agent, float dt);

    std::vector<NetAgent> agents_;
    EntryRule rule_;
    // Per-tick index: agents by piece, rebuilt at the start of tick().
    std::vector<std::vector<u32>> by_track_;
    std::vector<std::vector<u32>> by_connector_;
    std::vector<std::vector<u32>> by_change_;
};

} // namespace rw
