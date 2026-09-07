#pragma once

// What a lane is for, and which lane may go where at a junction.
//
// The engine's network carries a free 32-bit tag per track; the game puts the
// lane kind in it, so the network moves cars along lanes without ever knowing
// that a lane is a thing. Everything here is pure logic over the network, so
// it runs in a test without a window.

#include <rewind/sim/network.h>

#include <algorithm>
#include <cmath>

namespace traffic {

using rw::CurveNetwork;
using rw::NetEdge;
using rw::Path;
using rw::Track;
using rw::TrackRef;
using rw::u32;
using rw::usize;
using rw::Vec2;

/// What a lane is for. Stored in Track::tag.
enum class LaneKind : u32 {
    Driving = 0,
    LeftTurn,  ///< a pocket only left turners may use
    RightTurn,
    Bus,
    Cycle,
    Parking,
    Shoulder,
    Count
};

constexpr const char* kLaneKindNames[] = {"driving", "left", "right", "bus", "cycle", "parking", "shoulder"};

inline LaneKind lane_kind_from(const std::string& name) {
    for (u32 i = 0; i < static_cast<u32>(LaneKind::Count); ++i)
        if (name == kLaneKindNames[i]) return static_cast<LaneKind>(i);
    return LaneKind::Driving;
}

inline LaneKind kind_of(const Track& t) {
    return t.tag < static_cast<u32>(LaneKind::Count) ? static_cast<LaneKind>(t.tag) : LaneKind::Driving;
}

/// Can a vehicle drive here at all?
inline bool is_carriageway(LaneKind kind) {
    return kind == LaneKind::Driving || kind == LaneKind::LeftTurn || kind == LaneKind::RightTurn ||
           kind == LaneKind::Bus;
}

/// Forward lanes lie to the right of the centreline (positive offsets) and
/// backward lanes to the left, so each direction's kerb is the far side.
inline float kerb_side(bool forward) { return forward ? 1.0f : -1.0f; }

/// How far a lane is from its own kerb, counted in lanes: 0 is the kerb lane,
/// 1 the next one in, and so on. This is what stays put when a road gains or
/// loses a lane, so it is what decides which lane continues into which.
inline u32 rank_from_kerb(const NetEdge& e, u32 track) {
    if (track >= e.tracks.size()) return 0;
    const Track& lane = e.tracks[track];
    const float side = kerb_side(lane.forward);
    u32 rank = 0;
    for (const Track& t : e.tracks) {
        if (t.forward != lane.forward || !is_carriageway(kind_of(t))) continue;
        // A lane is further in than this one when it sits closer to the centre.
        if ((t.offset - lane.offset) * side > 0.01f) ++rank;
    }
    return rank;
}

/// Which way a movement turns: -1 left, 0 straight on, 1 right.
inline int turn_sense(const CurveNetwork& net, TrackRef from, TrackRef to) {
    const Path& in = net.track_path(from);
    const Path& out = net.track_path(to);
    if (in.empty() || out.empty()) return 0;
    const float turn = rw::cross(in.segments.back().direction(1), out.segments.front().direction(0));
    return std::fabs(turn) < 0.34f ? 0 : (turn > 0 ? 1 : -1);
}

/// Lane discipline. A turn pocket takes its own turn; the lane by the centre
/// line turns left, the lane by the kerb turns right, and a road with a
/// pocket keeps its through lanes clear of that turn. Going straight on, a
/// lane continues into the lane at the same distance from the kerb: that is
/// what makes a road that loses a lane a merge rather than a swerve, and a
/// road that gains one open a new lane rather than pull everyone across.
inline bool may_connect(const CurveNetwork& net, TrackRef from, TrackRef to) {
    if (!net.valid_edge(from.edge) || !net.valid_edge(to.edge)) return false;
    if (from.edge == to.edge) return false; // no turning back down the same road
    const NetEdge& in = net.edge(from.edge);
    const NetEdge& out = net.edge(to.edge);
    if (from.track >= in.tracks.size() || to.track >= out.tracks.size()) return false;

    const Track& lane = in.tracks[from.track];
    const Track& next = out.tracks[to.track];
    const LaneKind kind = kind_of(lane);
    const LaneKind target = kind_of(next);
    if (!is_carriageway(kind) || !is_carriageway(target)) return false;
    if (kind == LaneKind::Bus && target != LaneKind::Bus && target != LaneKind::Driving) return false;

    const int sense = turn_sense(net, from, to);
    if (kind == LaneKind::LeftTurn) return sense < 0;
    if (kind == LaneKind::RightTurn) return sense > 0;

    if (sense == 0) {
        // Straight on: the lane carries on into the lane the same distance
        // from the kerb. A lane with no such counterpart has ended, and the
        // cars in it have to move over before they get here.
        return rank_from_kerb(in, from.track) == rank_from_kerb(out, to.track);
    }

    // A plain lane may turn only from the side it is on, and only when the
    // road has no pocket for that turn.
    for (const Track& t : in.tracks) {
        if (t.forward != lane.forward) continue;
        const LaneKind other = kind_of(t);
        if ((sense < 0 && other == LaneKind::LeftTurn) || (sense > 0 && other == LaneKind::RightTurn)) return false;
    }
    u32 lanes = 0;
    for (const Track& t : in.tracks)
        if (t.forward == lane.forward && is_carriageway(kind_of(t))) ++lanes;
    const u32 rank = rank_from_kerb(in, from.track);
    return sense < 0 ? rank + 1 == lanes : rank == 0;
}

/// Does this lane carry on past the node it runs into, or does it end there?
inline bool lane_continues(const CurveNetwork& net, TrackRef track) {
    return !net.connectors_from(track).empty();
}

/// Does anything feed this lane at the node it starts from?
inline bool lane_is_fed(const CurveNetwork& net, TrackRef track) {
    for (const rw::Connector& c : net.connectors())
        if (c.to.edge == track.edge && c.to.track == track.track) return true;
    return false;
}

} // namespace traffic
