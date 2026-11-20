#pragma once

// Building roads: the shapes a road can take between two places, what the
// cursor snaps to, laying a road into the network so that junctions form
// where it meets others, and taking roads out again.
//
// Pure geometry over the engine's CurveNetwork with no renderer and no game
// state, so it runs in a test without a window.

#include "lanes.h"

#include <rewind/core/curve.h>

#include <string>

namespace roads {

using rw::PathSampler;

constexpr u32 kNone = ~0u;

// ---- road types ----------------------------------------------------------------------------

inline Track make_lane(float lanes_from_centre, bool forward, u32 use = kAny, float width = 3.5f) {
    return Track{lanes_from_centre * 3.5f, forward, width, use};
}

/// A kind of road: its lanes, left to right across it.
struct RoadType {
    const char* name;
    std::vector<Track> lanes;

    float width() const {
        float lo = 0, hi = 0;
        for (const Track& t : lanes) {
            lo = std::min(lo, t.offset - t.width * 0.5f);
            hi = std::max(hi, t.offset + t.width * 0.5f);
        }
        return std::max(hi, -lo) * 2;
    }
};

inline const std::vector<RoadType>& road_types() {
    static const std::vector<RoadType> types = {
        {"Street", {make_lane(-0.5f, false), make_lane(0.5f, true)}},
        {"Avenue", {make_lane(-1.5f, false), make_lane(-0.5f, false), make_lane(0.5f, true), make_lane(1.5f, true)}},
        {"Boulevard",
         {make_lane(-2.5f, false), make_lane(-1.5f, false), make_lane(-0.5f, false), make_lane(0.5f, true), make_lane(1.5f, true),
          make_lane(2.5f, true)}},
        {"One-way", {make_lane(0, true)}},
        {"One-way x2", {make_lane(-0.5f, true), make_lane(0.5f, true)}},
    };
    return types;
}

/// Give an edge the lanes of a road type. The type is remembered in the
/// edge's tag (plus one, so 0 means "not from the catalogue").
inline void apply_type(CurveNetwork& net, u32 edge, u32 type) {
    if (!net.valid_edge(edge) || type >= road_types().size()) return;
    NetEdge& e = net.edge(edge);
    e.tracks = road_types()[type].lanes;
    e.width = road_types()[type].width();
    e.tag = type + 1;
}

// ---- lanes, one at a time ------------------------------------------------------------------------

constexpr u32 kMostLanesOneWay = 6;
constexpr float kPocketLength = 45, kPocketTaper = 18;

/// The road is as wide as its lanes reach from the centre line, both sides alike.
inline void fit_width(NetEdge& e) {
    float reach = 0;
    for (const Track& t : e.tracks) reach = std::max(reach, absf(t.offset) + t.width * 0.5f);
    e.width = reach * 2;
}

/// The lane furthest out among the whole lanes going one way; ~0u when there is none.
inline u32 outer_lane(const NetEdge& e, bool forward) {
    u32 found = ~0u;
    for (u32 i = 0; i < e.tracks.size(); ++i) {
        const Track& t = e.tracks[i];
        if (t.forward != forward || !t.whole()) continue;
        if (found == ~0u || (t.offset - e.tracks[found].offset) * (forward ? 1.0f : -1.0f) > 0) found = i;
    }
    return found;
}

inline u32 lanes_one_way(const NetEdge& e, bool forward) {
    u32 count = 0;
    for (const Track& t : e.tracks) count += t.forward == forward && t.whole() ? 1 : 0;
    return count;
}

/// The edge of the road on one side: how far the lanes of any kind reach.
inline float kerb_offset(const NetEdge& e, bool forward) {
    const float side = forward ? 1.0f : -1.0f;
    float reach = 0;
    for (const Track& t : e.tracks) reach = std::max(reach, t.offset * side + t.width * 0.5f);
    return reach;
}

/// One more lane on the kerb side of a direction. False when the road is full.
inline bool add_lane(CurveNetwork& net, u32 edge, bool forward) {
    if (!net.valid_edge(edge)) return false;
    NetEdge& e = net.edge(edge);
    if (lanes_one_way(e, forward) >= kMostLanesOneWay) return false;
    // Outside every whole lane, but inside a pocket if there is one: that stays at the kerb.
    const float side = forward ? 1.0f : -1.0f;
    float reach = 0;
    for (const Track& t : e.tracks)
        if (t.whole()) reach = std::max(reach, t.offset * side + t.width * 0.5f);
    for (Track& t : e.tracks)
        if (!t.whole() && t.offset * side > reach - 0.1f) t.offset += 3.5f * side;
    e.tracks.push_back(Track{(reach + 1.75f) * side, forward, 3.5f, kAny});
    fit_width(e);
    return true;
}

/// Take the kerb lane of a direction away. The last lane of a road stays.
inline bool remove_lane(CurveNetwork& net, u32 edge, bool forward) {
    if (!net.valid_edge(edge)) return false;
    NetEdge& e = net.edge(edge);
    const u32 lane = outer_lane(e, forward);
    usize whole = 0;
    for (const Track& t : e.tracks) whole += t.whole() ? 1 : 0;
    if (lane == ~0u || whole <= 1) return false;
    const float side = forward ? 1.0f : -1.0f;
    const float gone = e.tracks[lane].offset * side;
    e.tracks.erase(e.tracks.begin() + lane);
    for (Track& t : e.tracks)
        if (!t.whole() && t.offset * side > gone) t.offset -= 3.5f * side; // a pocket moves in with the kerb
    // No lane left this way: nothing for a pocket to open from either.
    if (lanes_one_way(e, forward) == 0) std::erase_if(e.tracks, [&](const Track& t) { return t.forward == forward; });
    fit_width(e);
    return true;
}

/// A short lane at the kerb for turning right, opening out of the kerb lane
/// before the junction a direction arrives at. False when the road is too
/// short for one or has one already.
inline bool add_pocket(CurveNetwork& net, u32 edge, bool forward) {
    if (!net.valid_edge(edge)) return false;
    NetEdge& e = net.edge(edge);
    const float length = e.path.length();
    if (length < kPocketLength + 25 || lanes_one_way(e, forward) == 0) return false;
    for (const Track& t : e.tracks)
        if (t.forward == forward && !t.whole()) return false;
    Track pocket{(kerb_offset(e, forward) + 1.75f) * (forward ? 1.0f : -1.0f), forward, 3.5f, kRightOnly};
    (forward ? pocket.begin : pocket.end) = length - kPocketLength;
    pocket.taper = kPocketTaper;
    e.tracks.push_back(pocket);
    fit_width(e);
    return true;
}

inline bool remove_pocket(CurveNetwork& net, u32 edge, bool forward) {
    if (!net.valid_edge(edge)) return false;
    NetEdge& e = net.edge(edge);
    const usize before = e.tracks.size();
    std::erase_if(e.tracks, [&](const Track& t) { return t.forward == forward && !t.whole(); });
    fit_width(e);
    return e.tracks.size() != before;
}

// ---- how far short of a junction an arm's lanes stop -----------------------------------------

/// One road arriving at a node, seen from the node.
struct Arm {
    u32 edge = 0;
    Vec2 dir;        ///< away from the node
    float angle = 0;
    float left = 0;  ///< how far its paving reaches either side of its centre line, looking away
    float right = 0;
    float length = 0;
};

inline std::vector<Arm> arms_of(const CurveNetwork& net, u32 node) {
    std::vector<Arm> arms;
    for (u32 index : net.edges_at(node)) {
        const NetEdge& e = net.edge(index);
        if (e.path.empty() || e.a == e.b) continue;
        const bool leaving = e.a == node;
        Arm arm;
        arm.edge = index;
        arm.dir = leaving ? e.path.segments.front().direction(0) : e.path.segments.back().direction(1) * -1.0f;
        if (rw::length_sq(arm.dir) < 1e-6f) continue;
        arm.angle = rw::angle_of(arm.dir);
        arm.length = e.path.length();
        float lo = 0, hi = 0;
        for (const Track& t : e.tracks) {
            if (leaving ? t.begin > 0 : t.end > 0) continue; // does not reach this end
            lo = std::min(lo, t.offset - t.width * 0.5f);
            hi = std::max(hi, t.offset + t.width * 0.5f);
        }
        // Offsets are across the edge's own direction; an arm that leaves
        // the node against it sees them mirrored.
        arm.right = leaving ? hi : -lo;
        arm.left = leaving ? -lo : hi;
        arms.push_back(arm);
    }
    std::sort(arms.begin(), arms.end(), [](const Arm& a, const Arm& b) { return a.angle != b.angle ? a.angle < b.angle : a.edge < b.edge; });
    return arms;
}

/// The same lanes on both sides of a node with two roads: a seam in one
/// road rather than a change of road.
inline bool same_section(const CurveNetwork& net, u32 node, u32 edge_a, u32 edge_b) {
    struct Seen {
        float offset;
        bool away;
        float width;
        u32 tag;
    };
    const auto seen_from = [&](u32 edge, bool mirrored, std::vector<Seen>& out) {
        const NetEdge& e = net.edge(edge);
        const bool leaving = e.a == node;
        for (const Track& t : e.tracks) {
            if (!t.whole()) return false;
            Seen s{leaving ? t.offset : -t.offset, leaving ? t.forward : !t.forward, t.width, t.tag};
            if (mirrored) { // the road carrying straight on, looked at from the other side
                s.offset = -s.offset;
                s.away = !s.away;
            }
            out.push_back(s);
        }
        std::sort(out.begin(), out.end(), [](const Seen& x, const Seen& y) { return x.offset < y.offset; });
        return true;
    };
    std::vector<Seen> a, b;
    if (!seen_from(edge_a, false, a) || !seen_from(edge_b, true, b) || a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (absf(a[i].offset - b[i].offset) > 0.01f || a[i].away != b[i].away || absf(a[i].width - b[i].width) > 0.01f ||
            a[i].tag != b[i].tag)
            return false;
    return true;
}

/// How much one road bends at a node it shares with one other: 0 for none.
inline float kink_at(const std::vector<Arm>& arms) {
    if (arms.size() != 2) return 0;
    return std::acos(rw::clampf(rw::dot(arms[0].dir, arms[1].dir * -1.0f), -1, 1));
}

/// How far short of a node the lanes of one of its roads stop, so that the
/// movements through the junction have room. Worked out for each road by
/// itself, from where its kerbs meet its neighbours': a side road runs right
/// up to the main road however wide that is, and a sharp corner needs more
/// room than a square one. Give this to CurveNetwork::set_track_trim.
inline float arm_trim(const CurveNetwork& net, u32 node, u32 edge) {
    const std::vector<Arm> arms = arms_of(net, node);
    usize i = arms.size();
    for (usize k = 0; k < arms.size(); ++k)
        if (arms[k].edge == edge) i = k;
    if (i == arms.size() || arms.size() < 2) return 0;
    const Arm& arm = arms[i];
    float trim = 0;
    if (arms.size() == 2) {
        // A seam: nothing where the road simply carries on, a little where
        // it bends, and a proper run where its lanes change.
        const Arm& other = arms[1 - i];
        const float kink = kink_at(arms);
        const float half = std::max(arm.left, arm.right);
        if (kink > 0.03f) trim = half * std::tan(std::min(kink, 2.6f) * 0.5f) + 4.0f * kink + 1.0f;
        if (!same_section(net, node, arm.edge, other.edge)) trim = std::max(trim, 14.0f);
    } else {
        const Arm& next = arms[(i + 1) % arms.size()];
        const Arm& prev = arms[(i + arms.size() - 1) % arms.size()];
        // Where this road's kerb meets the kerb of the road beside it, on
        // each side. Angles grow clockwise on screen, so the next road round
        // is on this one's right.
        const auto meets = [](float mine, float theirs, float angle) {
            if (angle <= 0) angle += 6.2831853f;
            if (angle >= 3.0f) return 0.0f; // beside it or behind: the kerbs never cross in front
            return std::min((theirs + mine * std::cos(angle)) / std::max(std::sin(angle), 0.05f), 40.0f);
        };
        const float right = meets(arm.right, next.left, next.angle - arm.angle);
        const float left = meets(arm.left, prev.right, arm.angle - prev.angle);
        // Past that point, a few metres for the corner and for turning in.
        trim = rw::clampf(std::max({right, left, 0.0f}) + 3.5f, 5.0f, 45.0f);
    }
    // Never more than the road has: two junctions close together share it.
    return std::min(trim, arm.length * 0.45f);
}

// ---- shapes ----------------------------------------------------------------------------------

inline Path straight(Vec2 a, Vec2 b) {
    Path p;
    p.move_to(a);
    p.line_to(b);
    return p;
}

/// The arc that leaves `from` along `dir` and ends at `to`: the one bend a
/// road can make without a kink at its start. A straight when the two are
/// already in line. `radius_out` receives the radius it used, 0 for a straight.
inline Path arc_from(Vec2 from, Vec2 dir, Vec2 to, float* radius_out = nullptr) {
    Path path;
    if (radius_out) *radius_out = 0;
    const Vec2 chord = to - from;
    const float span = rw::length(chord);
    if (span < 0.01f || rw::length_sq(dir) < 1e-6f) return path;
    dir = rw::normalize(dir);
    path.move_to(from);
    const Vec2 side = rw::perp(dir);
    const float denom = 2.0f * rw::dot(side, chord);
    if (absf(denom) < 1e-3f * span) {
        if (rw::dot(dir, chord) > 0) path.line_to(to);
        else path.segments.clear(); // straight back the way it came: no arc does that
        return path;
    }
    const float radius = rw::length_sq(chord) / denom; // signed: which side the centre is on
    const Vec2 centre = from + side * radius;
    float sweep = rw::angle_of(to - centre) - rw::angle_of(from - centre);
    while (sweep > 3.14159265f) sweep -= 6.2831853f;
    while (sweep < -3.14159265f) sweep += 6.2831853f;
    // The arc has to set off along `dir`, not against it.
    const Vec2 start_tangent = rw::perp(from - centre) * (sweep > 0 ? 1.0f : -1.0f);
    if (rw::dot(start_tangent, dir) < 0) sweep += sweep > 0 ? -6.2831853f : 6.2831853f;
    path.arc_to(centre, sweep);
    if (radius_out) *radius_out = absf(radius);
    return path;
}

/// A road that leaves `a` heading `da` and arrives at `b` heading `db`, with
/// no kink at either end: an S where the two are offset, a bend where they
/// point different ways, a hairpin where they face each other. This is what
/// joins two road ends.
inline Path connect(Vec2 a, Vec2 da, Vec2 b, Vec2 db) {
    Path path;
    const float d = rw::distance(a, b);
    if (d < 0.01f) return path;
    da = rw::normalize(da);
    db = rw::normalize(db);
    // How far each end keeps its heading before giving way: about a third
    // of the distance for a gentle S, two thirds for a hairpin.
    const float turn = (1.0f - rw::dot(da, db)) * 0.5f;
    const float reach = d * (0.36f + 0.06f * turn + 0.25f * turn * turn * turn);
    path.segments.push_back({a, a + da * reach, b - db * reach, b});
    return path;
}

/// The tightest bend a road of this width may make: a lane can turn in ten
/// metres, a six-lane boulevard needs thirty.
inline float min_radius_for(float width) { return std::max(10.0f, width * 1.3f + 2.0f); }

/// The tightest bend anywhere along a path, as a radius. Huge for a straight.
inline float tightest_radius(const Path& path) {
    float most = 0;
    for (const rw::CubicBezier& s : path.segments)
        for (int i = 0; i <= 16; ++i) most = std::max(most, absf(s.curvature(static_cast<float>(i) / 16)));
    return most > 1e-6f ? 1.0f / most : 1e9f;
}

// ---- what the cursor snaps to ------------------------------------------------------------------

/// A place a road can start or end.
struct Snap {
    enum Kind : u32 {
        Free,  ///< open ground
        Grid,  ///< open ground, on the grid
        Guide, ///< open ground, on an angle step from the start
        Edge,  ///< on a road: a junction will form
        Node,  ///< an existing junction
        End,   ///< the open end of a road: it can simply be carried on
    };
    Vec2 pos;
    u32 kind = Free;
    u32 node = kNone;
    u32 edge = kNone;
    /// Which way a road leaves this place, when the place says: a road
    /// carried on from an open end leaves the way that road was heading.
    bool has_dir = false;
    Vec2 dir;

    bool on_network() const { return kind == Edge || kind == Node || kind == End; }

    void reflect(rw::Visitor& v) {
        v.field("pos", pos);
        v.field("kind", kind);
        v.field("node", node);
        v.field("edge", edge);
        v.field("has_dir", has_dir);
        v.field("dir", dir);
    }
};

/// The nearest thing on the network the cursor is close enough to stick to.
/// Open ends and junctions pull from further away than the middle of a road,
/// and a place close to the end of a road is that end.
inline Snap snap_to_network(const CurveNetwork& net, Vec2 cursor, float node_reach, float edge_reach) {
    Snap snap;
    snap.pos = cursor;
    const auto at_node = [&](u32 node) {
        snap.pos = net.node(node).pos;
        snap.node = node;
        const std::vector<Arm> arms = arms_of(net, node);
        if (arms.size() == 1) {
            snap.kind = Snap::End;
            snap.has_dir = true;
            snap.dir = arms[0].dir * -1.0f; // carrying on the way the road was going
        } else {
            snap.kind = Snap::Node;
        }
    };
    const u32 node = net.nearest_node(cursor, node_reach);
    if (node != kNone && net.valid_node(node)) {
        at_node(node);
        return snap;
    }
    const u32 edge = net.nearest_edge(cursor, edge_reach);
    if (edge == kNone) return snap;
    const PathSampler sampler(net.edge(edge).path, 0.1f);
    const rw::PathPoint hit = sampler.closest(cursor);
    // Too near an end to make a junction of its own: it is that end.
    const float margin = std::min(12.0f, sampler.length() * 0.3f);
    if (hit.distance < margin) at_node(net.edge(edge).a);
    else if (hit.distance > sampler.length() - margin) at_node(net.edge(edge).b);
    else {
        snap.kind = Snap::Edge;
        snap.edge = edge;
        snap.pos = hit.position;
    }
    return snap;
}

/// The point on a road nearest to `from`: where a straight road from there
/// meets it square.
inline Vec2 square_onto(const CurveNetwork& net, u32 edge, Vec2 from) {
    const PathSampler sampler(net.edge(edge).path, 0.1f);
    return sampler.closest(from).position;
}

// ---- planning a road ---------------------------------------------------------------------------

enum class Shape : u32 { Straight, Curve };

struct Plan {
    Path path;
    float length = 0;
    float radius = 0;      ///< tightest bend; 0 for a straight
    bool ok = false;
    const char* problem = ""; ///< why not, when not
    bool joined = false;   ///< it runs from one road end into another without a kink
};

/// The road between two places. Two open ends are joined so that neither has
/// a kink, whatever the shape asked for; a curve leaves along the heading of
/// whichever end has one; anything else is a straight.
inline Plan plan_road(const Snap& from, const Snap& to, Shape shape, float width) {
    Plan plan;
    if (rw::distance(from.pos, to.pos) < 0.5f) {
        plan.problem = "too short";
        return plan;
    }
    if (from.has_dir && to.has_dir) {
        plan.path = connect(from.pos, from.dir, to.pos, to.dir * -1.0f);
        plan.joined = true;
    } else if (shape == Shape::Curve && from.has_dir) {
        plan.path = arc_from(from.pos, from.dir, to.pos);
    } else if (shape == Shape::Curve && to.has_dir) {
        plan.path = arc_from(to.pos, to.dir, from.pos).reversed();
    } else {
        plan.path = straight(from.pos, to.pos);
    }
    if (plan.path.empty()) {
        plan.problem = "cannot turn back on itself";
        return plan;
    }
    plan.length = plan.path.length();
    const float tightest = tightest_radius(plan.path);
    plan.radius = tightest < 1e6f ? tightest : 0.0f;
    if (plan.length < 10.0f) plan.problem = "too short";
    else if (plan.radius > 0 && plan.radius < min_radius_for(width)) plan.problem = "bend too tight";
    else plan.ok = true;
    return plan;
}

// ---- laying a road -----------------------------------------------------------------------------

/// Where a new road meets an existing one.
struct Crossing {
    float along = 0;     ///< metres along the new road
    u32 edge = kNone;    ///< the existing road being crossed
    float on_edge = 0;   ///< metres along that road
    u32 node = kNone;    ///< an existing junction, when the crossing lands on one
    Vec2 at{};
};

struct BuildResult {
    bool ok = false;
    const char* refusal = "";
    std::vector<u32> edges;     ///< the pieces of the new road, in order
    std::vector<u32> junctions; ///< every node it touches, new and reused
    float length = 0;
};

inline bool closest_on_edge(const CurveNetwork& net, u32 edge, Vec2 p, float max_distance, float& out_distance) {
    if (!net.valid_edge(edge)) return false;
    const PathSampler s(net.edge(edge).path, 0.25f);
    if (s.empty()) return false;
    const rw::PathPoint hit = s.closest(p);
    if (rw::distance(hit.position, p) > max_distance) return false;
    out_distance = hit.distance;
    return true;
}

/// Every place the new road crosses an existing one, in order along the new
/// road. Crossings within `merge` metres of each other collapse into one, so
/// a road grazing a junction does not produce a shower of tiny edges.
inline std::vector<Crossing> find_crossings(const CurveNetwork& net, const Path& path, float snap, float merge = 6.0f) {
    std::vector<Crossing> found;
    std::vector<Vec2> line;
    path.flatten(0.25f, line);
    if (line.size() < 2) return found;
    const PathSampler sampler(path, 0.25f);
    const rw::Rect box = path.bounds();

    std::vector<Vec2> other;
    for (u32 e = 0; e < static_cast<u32>(net.edges().size()); ++e) {
        if (!net.valid_edge(e)) continue;
        const rw::Rect theirs = net.edge(e).path.bounds();
        if (theirs.x > box.x + box.w || theirs.x + theirs.w < box.x || theirs.y > box.y + box.h || theirs.y + theirs.h < box.y) continue;
        other.clear();
        net.edge(e).path.flatten(0.25f, other);
        if (other.size() < 2) continue;
        for (usize i = 1; i < line.size(); ++i) {
            for (usize j = 1; j < other.size(); ++j) {
                Vec2 where;
                if (!rw::segments_intersect(line[i - 1], line[i], other[j - 1], other[j], &where)) continue;
                Crossing c;
                c.at = where;
                c.along = sampler.closest(where).distance;
                c.edge = e;
                if (!closest_on_edge(net, e, where, snap + 1.0f, c.on_edge)) continue;
                // Landing on an end of the existing road means the junction is
                // already there: reuse it instead of splitting.
                const NetEdge& edge = net.edge(e);
                const float len = edge.path.length();
                if (c.on_edge < snap) c.node = edge.a;
                else if (c.on_edge > len - snap) c.node = edge.b;
                found.push_back(c);
            }
        }
    }

    std::sort(found.begin(), found.end(), [](const Crossing& a, const Crossing& b) {
        return a.along != b.along ? a.along < b.along : a.edge < b.edge;
    });
    std::vector<Crossing> merged;
    for (const Crossing& c : found) {
        if (!merged.empty() && c.along - merged.back().along < merge && c.edge == merged.back().edge) continue;
        merged.push_back(c);
    }
    return merged;
}

/// Split an edge `distance` metres along, returning the new node.
inline u32 split_at(CurveNetwork& net, u32 edge, float distance) {
    if (!net.valid_edge(edge)) return kNone;
    const PathSampler s(net.edge(edge).path, 0.25f);
    const float len = s.length();
    if (len < 2 || distance < 1.0f || distance > len - 1.0f) return kNone;
    const rw::PathPoint at = s.at(distance);
    const u32 node = net.split_edge(edge, at.segment, at.t);
    if (node != kNone) net.node(node).tag = 0;
    return node;
}

/// Lay `path` into the network as a road of the given type. Its ends snap to
/// nearby junctions, or split the road they land on; everything it crosses on
/// the way is split too, and the new road is added as one piece between each
/// pair of junctions. Call refresh() afterwards.
inline BuildResult lay_road(CurveNetwork& net, const Path& path, u32 type, float snap = 3.0f, float min_length = 8.0f) {
    BuildResult result;
    if (path.empty()) {
        result.refusal = "nothing drawn";
        return result;
    }
    const float total = path.length();
    result.length = total;
    if (total < min_length) {
        result.refusal = "too short";
        return result;
    }
    const Vec2 start = path.start();
    const Vec2 end = path.end();

    // Ends first: an existing junction wins, otherwise a road to split,
    // otherwise a new junction of our own.
    struct End {
        u32 node = kNone;
        u32 edge = kNone;
        float on_edge = 0;
    };
    auto resolve = [&](Vec2 at) {
        End e;
        e.node = net.nearest_node(at, snap);
        if (e.node != kNone) return e;
        const u32 edge = net.nearest_edge(at, snap);
        float distance = 0;
        if (edge != kNone && closest_on_edge(net, edge, at, snap, distance)) {
            e.edge = edge;
            e.on_edge = distance;
        }
        return e;
    };
    End head = resolve(start);
    End tail = resolve(end);
    if (head.node != kNone && head.node == tail.node) {
        result.refusal = "starts and ends at the same junction";
        return result;
    }

    std::vector<Crossing> crossings = find_crossings(net, path, snap);
    // The ends are handled separately, so drop crossings that sit on them.
    crossings.erase(std::remove_if(crossings.begin(), crossings.end(),
                                   [&](const Crossing& c) { return c.along < snap + 1.0f || c.along > total - snap - 1.0f; }),
                    crossings.end());

    // Split from the far end of each road backwards, so the distances of the
    // crossings still to come stay on the piece that keeps its index.
    struct Split {
        u32 edge;
        float on_edge;
        usize slot; ///< index into `crossings`, or one of the two ends
    };
    constexpr usize kHead = ~usize(0), kTail = ~usize(0) - 1;
    std::vector<Split> splits;
    for (usize i = 0; i < crossings.size(); ++i)
        if (crossings[i].node == kNone) splits.push_back(Split{crossings[i].edge, crossings[i].on_edge, i});
    if (head.node == kNone && head.edge != kNone) splits.push_back(Split{head.edge, head.on_edge, kHead});
    if (tail.node == kNone && tail.edge != kNone) splits.push_back(Split{tail.edge, tail.on_edge, kTail});
    std::sort(splits.begin(), splits.end(), [](const Split& a, const Split& b) {
        return a.edge != b.edge ? a.edge < b.edge : a.on_edge > b.on_edge;
    });
    for (const Split& s : splits) {
        const u32 node = split_at(net, s.edge, s.on_edge);
        if (node == kNone) continue;
        if (s.slot == kHead) head.node = node;
        else if (s.slot == kTail) tail.node = node;
        else crossings[s.slot].node = node;
    }

    if (head.node == kNone) head.node = net.add_node(start);
    if (tail.node == kNone) tail.node = net.add_node(end);
    if (head.node == tail.node) {
        result.refusal = "starts and ends at the same junction";
        return result;
    }

    // Lay the road as one piece between each pair of junctions.
    std::vector<std::pair<float, u32>> stops;
    stops.emplace_back(0.0f, head.node);
    for (const Crossing& c : crossings)
        if (c.node != kNone) stops.emplace_back(c.along, c.node);
    stops.emplace_back(total, tail.node);
    std::sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    for (usize i = 1; i < stops.size(); ++i) {
        const float from = stops[i - 1].first;
        const float to = stops[i].first;
        const u32 a = stops[i - 1].second;
        const u32 b = stops[i].second;
        if (a == b || to - from < 1.0f) continue;
        Path piece = path.sub_path(from, to, 0.02f);
        if (piece.empty()) continue;
        const u32 edge = net.add_edge(a, b, &piece);
        if (edge == kNone) continue;
        apply_type(net, edge, type);
        result.edges.push_back(edge);
        if (result.junctions.empty()) result.junctions.push_back(a);
        result.junctions.push_back(b);
    }
    if (result.edges.empty()) {
        result.refusal = "nothing to connect";
        return result;
    }
    result.ok = true;
    return result;
}

// ---- what may be built ---------------------------------------------------------------------------

/// The limits a road has to keep to. They are what keeps every junction
/// something that can be drawn well and driven through: nothing here stops a
/// player building any kind of junction, only a kind nobody could build.
struct Limits {
    float min_length = 10;   ///< metres
    float min_corner = 30;   ///< degrees between a road and any road it joins
    float min_crossing = 35; ///< degrees at which it may cross another
    u32 max_arms = 6;        ///< roads at one junction
    float min_spacing = 12;  ///< metres between two junctions along one road
};

inline float degrees_between(Vec2 a, Vec2 b) { return std::acos(rw::clampf(rw::dot(a, b), -1, 1)) * 57.29578f; }

/// Why a road along `path` cannot be built, or "" when it can.
inline const char* road_problem(const CurveNetwork& net, const Path& path, float width, const Limits& limits = {}) {
    if (path.empty()) return "nothing drawn";
    const float total = path.length();
    if (total < limits.min_length) return "too short";
    if (tightest_radius(path) < min_radius_for(width)) return "bend too tight";

    // It may not cross itself.
    std::vector<Vec2> line;
    path.flatten(0.5f, line);
    for (usize i = 0; i + 1 < line.size(); ++i)
        for (usize j = i + 2; j + 1 < line.size(); ++j)
            if (rw::segments_intersect(line[i], line[i + 1], line[j], line[j + 1])) return "crosses itself";

    // Where it joins another road, at either end, it joins at a proper angle.
    const PathSampler sampler(path, 0.25f);
    bool joins[2] = {false, false};
    for (int end = 0; end < 2; ++end) {
        const Vec2 at = end == 0 ? path.start() : path.end();
        const Vec2 away = end == 0 ? path.segments.front().direction(0) : path.segments.back().direction(1) * -1.0f;
        const u32 node = net.nearest_node(at, 3.0f);
        if (node != kNone && net.valid_node(node)) {
            const std::vector<Arm> arms = arms_of(net, node);
            if (arms.size() + 1 > limits.max_arms) return "too many roads at one junction";
            for (const Arm& arm : arms)
                if (degrees_between(arm.dir, away) < limits.min_corner) return "too sharp an angle to another road";
            joins[end] = !arms.empty();
            continue;
        }
        const u32 edge = net.nearest_edge(at, 3.0f);
        if (edge == kNone) continue;
        const PathSampler theirs(net.edge(edge).path, 0.25f);
        const rw::PathPoint hit = theirs.closest(at);
        if (rw::distance(hit.position, at) > 3.0f) continue;
        joins[end] = true;
        const float angle = degrees_between(hit.direction, away);
        if (std::min(angle, 180.0f - angle) < limits.min_corner) return "meets the road at too flat an angle";
        const NetEdge& e = net.edge(edge);
        if ((hit.distance < limits.min_spacing && net.edges_at(e.a).size() >= 3) ||
            (theirs.length() - hit.distance < limits.min_spacing && net.edges_at(e.b).size() >= 3))
            return "too close to another junction";
    }

    // Where it crosses a road on the way, likewise, and not twice in a few metres.
    const std::vector<Crossing> crossings = find_crossings(net, path, 3.0f);
    float last = joins[0] ? 0.0f : -1e9f;
    for (const Crossing& c : crossings) {
        if (c.along < 4.0f || c.along > total - 4.0f) continue; // the ends, looked at above
        const PathSampler theirs(net.edge(c.edge).path, 0.25f);
        const float angle = degrees_between(theirs.at(c.on_edge).direction, sampler.at(c.along).direction);
        if (std::min(angle, 180.0f - angle) < limits.min_crossing) return "crosses a road at too flat an angle";
        if (c.along - last < limits.min_spacing) return "junctions too close together";
        last = c.along;
        const NetEdge& e = net.edge(c.edge);
        if (c.node != kNone) {
            if (arms_of(net, c.node).size() + 2 > limits.max_arms) return "too many roads at one junction";
        } else if ((c.on_edge < limits.min_spacing && net.edges_at(e.a).size() >= 3) ||
                   (theirs.length() - c.on_edge < limits.min_spacing && net.edges_at(e.b).size() >= 3)) {
            return "too close to another junction";
        }
    }
    if (joins[1] && last > 0 && total - last < limits.min_spacing) return "junctions too close together";

    // And it does not run along another road so close that the two would
    // share their paving: beside one, and heading the same way.
    for (float d = 6.0f; d <= total - 6.0f; d += 3.0f) {
        const rw::PathPoint mine = sampler.at(d);
        const u32 edge = net.nearest_edge(mine.position, width * 0.5f + 11.0f);
        if (edge == kNone) continue;
        const PathSampler theirs(net.edge(edge).path, 0.25f);
        const rw::PathPoint hit = theirs.closest(mine.position);
        if (hit.distance < 0.5f || hit.distance > theirs.length() - 0.5f) continue; // past its end, not beside it
        if (absf(rw::dot(hit.direction, mine.direction)) < 0.94f) continue;        // crossing or leaving, not alongside
        if (rw::distance(hit.position, mine.position) < (width + net.edge(edge).width) * 0.5f + 0.5f)
            return "too close alongside another road";
    }
    return "";
}

/// Why a junction is not one that may exist, or "" when it is. For checking
/// what moving a junction or bending a road has done to it.
inline const char* node_problem(const CurveNetwork& net, u32 node, const Limits& limits = {}) {
    if (!net.valid_node(node)) return "";
    const std::vector<Arm> arms = arms_of(net, node);
    if (arms.size() > limits.max_arms) return "too many roads at one junction";
    for (usize i = 0; i < arms.size(); ++i) {
        for (usize j = i + 1; j < arms.size(); ++j)
            if (degrees_between(arms[i].dir, arms[j].dir) < limits.min_corner * 0.8f) return "too sharp an angle between roads";
        const NetEdge& e = net.edge(arms[i].edge);
        if (e.path.length() < limits.min_length * 0.8f) return "too short";
        if (tightest_radius(e.path) < min_radius_for(e.width) * 0.8f) return "bend too tight";
    }
    return "";
}

// ---- tidying after an edit -----------------------------------------------------------------------

/// Turn an edge round: the same road, described from its other end.
inline void reverse_edge(CurveNetwork& net, u32 edge) {
    NetEdge& e = net.edge(edge);
    e.path = e.path.reversed();
    std::swap(e.a, e.b);
    for (Track& t : e.tracks) {
        t.offset = -t.offset;
        t.forward = !t.forward;
        std::swap(t.begin, t.end);
    }
}

/// A node where one road simply carries on, with the same lanes and no bend,
/// is not a junction: the two pieces become one road again. True when it did.
inline bool dissolve(CurveNetwork& net, u32 node) {
    if (!net.valid_node(node) || net.node(node).authored) return false;
    const std::vector<u32> edges = net.edges_at(node);
    if (edges.size() != 2 || edges[0] == edges[1]) return false;
    const u32 first = edges[0], second = edges[1];
    if (net.edge(first).a == net.edge(first).b || net.edge(second).a == net.edge(second).b) return false;
    if (net.edge(first).tag != net.edge(second).tag) return false;
    if (!same_section(net, node, first, second)) return false;
    if (kink_at(arms_of(net, node)) > 0.03f) return false;
    // A loop of two edges would become an edge from a node to itself.
    const u32 far_a = net.edge(first).a == node ? net.edge(first).b : net.edge(first).a;
    const u32 far_b = net.edge(second).a == node ? net.edge(second).b : net.edge(second).a;
    if (far_a == far_b) return false;

    if (net.edge(first).a == node) reverse_edge(net, first);   // first runs into the node
    if (net.edge(second).b == node) reverse_edge(net, second); // second runs out of it
    NetEdge& keep = net.edge(first);
    keep.path.join(net.edge(second).path);
    keep.b = net.edge(second).b;
    net.remove_edge(second);
    net.remove_node(node);
    return true;
}

/// After any edit: drop junctions nothing leads to, make one road of pieces
/// that simply carry on, and rebuild. Every junction's radius is set to the
/// longest of its arms' trims, for anything that wants to know how big it is.
inline void refresh(CurveNetwork& net) {
    const u32 count = static_cast<u32>(net.nodes().size());
    for (u32 n = 0; n < count; ++n)
        if (net.valid_node(n) && net.edges_at(n).empty()) net.remove_node(n);
    for (int pass = 0; pass < 8; ++pass) {
        bool any = false;
        for (u32 n = 0; n < count; ++n) any = dissolve(net, n) || any;
        if (!any) break;
    }
    for (u32 n = 0; n < count; ++n) {
        if (!net.valid_node(n)) continue;
        float radius = 0;
        for (u32 e : net.edges_at(n)) radius = std::max(radius, arm_trim(net, n, e));
        if (net.node(n).radius != radius) net.node(n).radius = radius;
    }
    net.rebuild();
}

/// Bend a road so that it passes through a point, keeping its ends.
inline void bend_through(CurveNetwork& net, u32 edge, Vec2 through) {
    if (!net.valid_edge(edge)) return;
    NetEdge& e = net.edge(edge);
    const Vec2 a = net.node(e.a).pos, b = net.node(e.b).pos;
    // A quadratic is half way along at t = 1/2, a quarter of the way from
    // the chord's middle to its control point short of it: put it there.
    const Vec2 control = through * 2.0f - (a + b) * 0.5f;
    Path path;
    path.segments.push_back(rw::CubicBezier::from_quadratic(a, control, b));
    e.path = path;
}

} // namespace roads
