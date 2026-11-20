#pragma once

// What a lane is for, and which lane may go where at a junction.
//
// Pure logic over the engine's CurveNetwork: no renderer, no game state, so
// it runs in a test without a window. Traffic keeps right throughout.

#include <rewind/sim/network.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace roads {

using rw::absf;
using rw::CurveNetwork;
using rw::NetEdge;
using rw::Path;
using rw::Track;
using rw::TrackRef;
using rw::u32;
using rw::usize;
using rw::Vec2;

/// What a lane is for, kept in Track::tag: the arrows painted on it. kAny
/// leaves it to the rules below; anything else is the set of turns the lane
/// may make, and nothing but those. A lane with arrows takes its turns away
/// from the lanes beside it that have none.
enum LaneUse : u32 {
    kAny = 0,
    kLeft = 1,
    kRight = 2,
    kStraight = 4,
    kLeftOnly = kLeft,
    kRightOnly = kRight,
};

/// A lane that may only turn: not one to carry on in, or to turn into.
inline bool turn_only(u32 use) { return use != kAny && (use & kStraight) == 0; }
/// Lanes that carry on into each other across a seam are of one kind: the
/// through lanes, or the lanes for one particular turn.
inline u32 lane_kind(const Track& t) { return turn_only(t.tag) ? t.tag : 0; }

inline float length_of(const std::vector<Vec2>& points) {
    float total = 0;
    for (usize i = 0; i + 1 < points.size(); ++i) total += rw::distance(points[i], points[i + 1]);
    return total;
}

constexpr int kBack = 2;

/// Which way a movement turns: -1 left, 0 straight on, 1 right, kBack for
/// doubling back, which nobody drives.
inline int turn_of(const CurveNetwork& n, TrackRef from, TrackRef to) {
    const Path& a = n.track_path(from);
    const Path& b = n.track_path(to);
    if (a.empty() || b.empty()) return kBack;
    const Vec2 d0 = a.segments.back().direction(1), d1 = b.segments.front().direction(0);
    const float along = rw::dot(d0, d1), across = rw::cross(d0, d1);
    if (along < -0.8f) return kBack;
    if (along > 0 && absf(across) < 0.45f) return 0;
    return across > 0 ? 1 : -1; // y grows downwards: clockwise is right
}

/// The lane next to a lane on one side, as its own traffic sees it: -1 is
/// its left, +1 its right. nullptr when there is none: kerb, or nothing.
inline const Track* beside(const NetEdge& edge, const Track& lane, float side) {
    // Across the road, a lane's right is +offset when it runs with the edge
    // and -offset when it runs against it.
    const float across = lane.forward ? side : -side;
    const Track* found = nullptr;
    for (const Track& other : edge.tracks)
        if (&other != &lane && (other.offset - lane.offset) * across > 0.01f &&
            (!found || absf(other.offset - lane.offset) < absf(found->offset - lane.offset)))
            found = &other;
    return found;
}

/// Where a lane sits among the lanes going its way that traffic may turn
/// into: 0 is the kerb lane. Those are the through lanes, or, on a road whose
/// every lane is a turn lane further on, all of them.
struct Lane {
    u32 rank = 0;
    u32 count = 0;
    bool counted = false; ///< is this lane one of them itself?
};

inline Lane lane_of(const NetEdge& e, u32 track) {
    Lane lane;
    const Track& t = e.tracks[track];
    const float kerb = t.forward ? 1.0f : -1.0f; // the kerb is on the right
    bool any_through = false;
    for (const Track& o : e.tracks) any_through = any_through || (o.forward == t.forward && o.whole() && !turn_only(o.tag));
    const auto member = [&](const Track& o) { return o.forward == t.forward && o.whole() && (!any_through || !turn_only(o.tag)); };
    for (const Track& o : e.tracks) {
        if (!member(o)) continue;
        ++lane.count;
        if ((o.offset - t.offset) * kerb > 0.01f) ++lane.rank;
    }
    lane.counted = member(t);
    return lane;
}

/// Does the road bring a lane with an arrow for this turn to this node?
inline bool has_turn_lane(const CurveNetwork& n, u32 node, u32 edge, bool forward, u32 use) {
    const NetEdge& e = n.edge(edge);
    for (u32 i = 0; i < e.tracks.size(); ++i)
        if (e.tracks[i].forward == forward && (e.tracks[i].tag & use) != 0 && n.arrives_at(TrackRef{edge, i}, node)) return true;
    return false;
}

/// The two arms of a junction that are both bigger than every other arm:
/// one road, whichever way it points. False when there is no such pair.
inline bool main_arms(const CurveNetwork& n, u32 node, u32& first, u32& second) {
    std::vector<u32> arms = n.edges_at(node);
    if (arms.size() < 3) return false;
    const auto lanes_of = [&](u32 edge) {
        u32 count = 0;
        for (const Track& t : n.edge(edge).tracks) count += t.whole() && !turn_only(t.tag) ? 1 : 0;
        return count;
    };
    std::stable_sort(arms.begin(), arms.end(), [&](u32 x, u32 y) { return lanes_of(x) > lanes_of(y); });
    if (lanes_of(arms[1]) <= lanes_of(arms[2])) return false;
    first = std::min(arms[0], arms[1]);
    second = std::max(arms[0], arms[1]);
    return true;
}

/// Lane discipline: which lane may go where.
///
/// Straight on, a lane carries on into the lane that lies in front of it: the
/// nearest one leaving the junction, which is the same lane on a road that
/// keeps its lanes, the one lane left where two become one, and the
/// acceleration lane for a slip road that arrives beside the main road.
/// Where the road ahead has more lanes, a lane that nothing would lead into
/// is fed by the lane nearest to it, so one lane opens out into two.
/// The kerb lane turns right and the lane by the centre line turns left,
/// unless the road has a lane for that turn, which then takes it alone.
/// Two turn lanes side by side turn side by side, each into its own lane.
///
/// All of that is what a lane without arrows does. The player may paint
/// arrows on a lane, and then it makes exactly those turns: any mix of left,
/// straight on and right.
inline bool may_connect(const CurveNetwork& n, u32 node, TrackRef from, TrackRef to) {
    if (from.edge == to.edge) return false;
    const int turn = turn_of(n, from, to);
    if (turn == kBack) return false;
    const NetEdge& in = n.edge(from.edge);
    const NetEdge& out = n.edge(to.edge);
    const Track& a = in.tracks[from.track];
    const Track& b = out.tracks[to.track];

    if (turn == 0) {
        if (n.track_path(from).empty() || n.track_path(to).empty()) return false;
        const u32 ka = lane_kind(a), kb = lane_kind(b);
        const Vec2 here = n.track_path(from).end(), there = n.track_path(to).start();
        // A turn lane carries on only into more of itself: the lane it has
        // been all along, straight ahead, not one across the junction.
        if (ka != 0 && absf(rw::cross(n.track_path(from).segments.back().direction(1), there - here)) > 1.75f) return false;
        const auto nearest_leaving = [&](Vec2 p, u32 kind) {
            u32 nearest = ~0u;
            float best = 1e30f;
            for (u32 j = 0; j < out.tracks.size(); ++j) {
                const TrackRef other{to.edge, j};
                if (lane_kind(out.tracks[j]) != kind || !n.leaves_from(other, node) || n.track_path(other).empty()) continue;
                const float d = rw::distance(n.track_path(other).start(), p);
                if (d < best) {
                    best = d;
                    nearest = j;
                }
            }
            return nearest;
        };
        const auto nearest_arriving = [&](Vec2 p, u32 kind) {
            u32 nearest = ~0u;
            float best = 1e30f;
            for (u32 i = 0; i < in.tracks.size(); ++i) {
                const TrackRef other{from.edge, i};
                if (lane_kind(in.tracks[i]) != kind || !n.arrives_at(other, node) || n.track_path(other).empty()) continue;
                const float d = rw::distance(n.track_path(other).end(), p);
                if (d < best) {
                    best = d;
                    nearest = i;
                }
            }
            return nearest;
        };
        if (ka == kb && nearest_leaving(here, ka) == to.track) return true;
        // The other way round: is this the lane nearest to a lane that
        // would otherwise begin from nothing?
        bool fed = false, own_kind_arrives = false;
        for (u32 i = 0; i < in.tracks.size(); ++i) {
            const TrackRef other{from.edge, i};
            if (lane_kind(in.tracks[i]) != kb || !n.arrives_at(other, node) || n.track_path(other).empty()) continue;
            own_kind_arrives = true;
            fed = fed || nearest_leaving(n.track_path(other).end(), kb) == to.track;
        }
        if (fed) return false;
        if (ka == kb) return nearest_arriving(there, ka) == from.track;
        // A turn lane that runs the whole length of its road has to be got
        // into somewhere: where no lane of its own kind leads into it, the
        // nearest through lane does.
        if (ka != 0 || own_kind_arrives || !b.whole()) return false;
        return nearest_arriving(there, 0) == from.track;
    }

    const u32 arrow = turn > 0 ? kRight : kLeft;
    const Lane la = lane_of(in, from.track), lb = lane_of(out, to.track);
    if (!lb.counted || lb.count == 0) return false;
    // A main road that turns the corner takes all its lanes round with it.
    u32 first = 0, second = 0;
    if (a.tag == kAny && a.whole() && main_arms(n, node, first, second) &&
        std::min(from.edge, to.edge) == first && std::max(from.edge, to.edge) == second)
        return std::min(la.rank, lb.count - 1) == lb.rank;
    if (a.tag != kAny) {
        if ((a.tag & arrow) == 0) return false;
        // Which of the lanes with this arrow is it, counted from the side it turns to?
        u32 place = 0;
        const float side = (turn > 0) == a.forward ? 1.0f : -1.0f;
        for (u32 i = 0; i < in.tracks.size(); ++i)
            if (i != from.track && in.tracks[i].forward == a.forward && (in.tracks[i].tag & arrow) != 0 &&
                n.arrives_at(TrackRef{from.edge, i}, node) && (in.tracks[i].offset - a.offset) * side > 0.01f)
                ++place;
        if (place >= lb.count) return false;
        return turn > 0 ? lb.rank == place : lb.rank + 1 + place == lb.count;
    }
    if (has_turn_lane(n, node, from.edge, a.forward, arrow)) return false;
    if (!a.whole() || !(turn > 0 ? la.rank == 0 : la.rank + 1 == la.count)) return false;
    return turn > 0 ? lb.rank == 0 : lb.rank + 1 == lb.count;
}

/// The arrows a lane has in effect at the junction it arrives at: the turns
/// it actually makes there, whether painted on it or worked out by the rules.
inline u32 arrows_of(const CurveNetwork& n, TrackRef lane) {
    u32 arrows = 0;
    for (u32 c : n.connectors_from(lane)) {
        const rw::Connector& k = n.connectors()[c];
        const int turn = turn_of(n, k.from, k.to);
        arrows |= turn == 0 ? kStraight : turn == 1 ? kRight : turn == -1 ? kLeft : 0;
    }
    return arrows;
}

/// Who has priority at a junction, worked out from the roads themselves.
///
/// A road that carries straight on through a junction is a through road
/// there. Two that share no arm cross each other, and then the one with
/// more lanes has priority; if they have the same, neither has, and the
/// junction is a crossing of equals. Two that share an arm are a fork. Lanes
/// that part or join without crossing dispute nothing: a slip road, a lane
/// that leaves. Where the branches do cross each other, the straighter one
/// is the main road, then the bigger one, and a fork with nothing to choose
/// between its branches is a junction of equals too. A road that only ends
/// at the junction never has priority.
///
/// The priority road may also turn the corner: two arms that are both bigger
/// than every other arm are one road, whichever way they point, and the
/// smaller ones join it on the bend.
struct Priority {
    std::vector<u32> edges; ///< the arms with priority
    bool equals = false;    ///< through roads cross here and none of them has priority
    bool bends = false;     ///< the priority road turns a corner here

    bool has(u32 edge) const { return std::find(edges.begin(), edges.end(), edge) != edges.end(); }
    /// Does this movement follow the priority road through the junction?
    bool along(const CurveNetwork& n, const rw::Connector& k) const {
        if (!has(k.from.edge) || !has(k.to.edge)) return false;
        return bends ? turn_of(n, k.from, k.to) != kBack : turn_of(n, k.from, k.to) == 0;
    }
};

/// How far sideways a movement ends from where carrying straight on would
/// have taken it: the offset between two arms that do not line up.
inline float shift_of(const CurveNetwork& n, const rw::Connector& k) {
    const Path& from = n.track_path(k.from);
    const Path& to = n.track_path(k.to);
    if (from.empty() || to.empty()) return 0;
    return absf(rw::cross(from.segments.back().direction(1), to.start() - from.end()));
}

inline Priority priority_at(const CurveNetwork& n, u32 node) {
    struct Through {
        u32 a, b;
        u32 lanes;
        float straight = 1; ///< 1 dead straight, less the more it bends
        std::vector<u32> connectors;
        bool beaten = false, rivalled = false;
    };
    const auto lanes_of = [&](u32 edge) {
        u32 count = 0;
        for (const Track& t : n.edge(edge).tracks) count += t.whole() && !turn_only(t.tag) ? 1 : 0;
        return count;
    };
    std::vector<Through> roads;
    for (u32 c : n.connectors_at(node)) {
        const rw::Connector& k = n.connectors()[c];
        if (turn_of(n, k.from, k.to) != 0) continue;
        const u32 a = std::min(k.from.edge, k.to.edge), b = std::max(k.from.edge, k.to.edge);
        Through* road = nullptr;
        for (Through& t : roads)
            if (t.a == a && t.b == b) road = &t;
        if (!road) {
            roads.push_back(Through{a, b, lanes_of(a) + lanes_of(b)});
            road = &roads.back();
        }
        road->connectors.push_back(c);
        const Path& from = n.track_path(k.from);
        const Path& to = n.track_path(k.to);
        road->straight = std::min(road->straight, rw::dot(from.segments.back().direction(1), to.segments.front().direction(0)));
    }
    const auto cross = [&](const Through& p, const Through& q) {
        for (u32 c : p.connectors)
            for (u32 i : n.conflicts_of(c)) {
                const rw::Conflict& x = n.conflicts()[i];
                if (x.kind != rw::ConflictKind::Crossing) continue;
                const u32 other = x.connector_a == c ? x.connector_b : x.connector_a;
                if (std::find(q.connectors.begin(), q.connectors.end(), other) != q.connectors.end()) return true;
            }
        return false;
    };
    for (Through& p : roads)
        for (const Through& q : roads) {
            if (&p == &q) continue;
            if (p.a == q.a || p.a == q.b || p.b == q.a || p.b == q.b) {
                if (!cross(p, q)) continue; // lanes parting or joining
                p.rivalled = true;
                if (q.straight > p.straight + 0.02f) p.beaten = true;
                else if (q.straight > p.straight - 0.02f && q.lanes >= p.lanes) p.beaten = true;
                continue;
            }
            p.rivalled = true;
            if (q.lanes >= p.lanes) p.beaten = true;
        }
    Priority out;
    bool disputed = false;
    for (const Through& p : roads) {
        disputed = disputed || p.rivalled;
        if (p.beaten) continue;
        if (!out.has(p.a)) out.edges.push_back(p.a);
        if (!out.has(p.b)) out.edges.push_back(p.b);
    }
    out.equals = disputed && out.edges.empty();
    u32 a = 0, b = 0;
    if (main_arms(n, node, a, b)) {
        bool straight = false, joined = false;
        for (const Through& t : roads) straight = straight || (t.a == a && t.b == b);
        for (u32 c : n.connectors_at(node)) {
            const rw::Connector& k = n.connectors()[c];
            joined = joined || (std::min(k.from.edge, k.to.edge) == a && std::max(k.from.edge, k.to.edge) == b);
        }
        if (!straight && joined) {
            out.edges = {a, b};
            out.bends = true;
            out.equals = false;
        }
    }
    return out;
}

/// A line with `head` taken off its start and `tail` off its end.
inline std::vector<Vec2> shortened(const std::vector<Vec2>& line, float head, float tail) {
    const float total = length_of(line);
    std::vector<Vec2> out;
    if (total <= head + tail + 0.5f) return out;
    float walked = 0;
    for (usize i = 0; i + 1 < line.size(); ++i) {
        const float len = rw::distance(line[i], line[i + 1]);
        const float from = std::max(head, walked), to = std::min(total - tail, walked + len);
        if (to > from && len > 1e-6f) {
            if (out.empty()) out.push_back(rw::lerp(line[i], line[i + 1], (from - walked) / len));
            out.push_back(rw::lerp(line[i], line[i + 1], (to - walked) / len));
        }
        walked += len;
    }
    return out;
}

inline bool crossing(const std::vector<Vec2>& a, const std::vector<Vec2>& b) {
    for (usize i = 0; i + 1 < a.size(); ++i)
        for (usize j = 0; j + 1 < b.size(); ++j)
            if (rw::segments_intersect(a[i], a[i + 1], b[j], b[j + 1])) return true;
    return false;
}

} // namespace roads
