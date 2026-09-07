#pragma once

// Laying a road into the network: snapping its ends, finding everything it
// crosses, splitting those roads so a junction appears at each crossing, and
// adding the new road as a chain of pieces between them.
//
// This is where a drawn line becomes a network rather than a picture. It is
// pure geometry over the engine's CurveNetwork with no renderer and no game
// state, so it runs in a test without a window.
//
// Header-only on purpose: the game and the test both just include it.

#include <rewind/core/curve.h>
#include <rewind/sim/network.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace traffic {

using rw::CurveNetwork;
using rw::NetEdge;
using rw::Path;
using rw::PathPoint;
using rw::PathSampler;
using rw::u32;
using rw::usize;
using rw::Vec2;

constexpr u32 kNoIndex = ~0u;

/// What kind of road to lay.
struct RoadSpec {
    u32 lanes_forward = 1;
    u32 lanes_backward = 1;
    float lane_width = 3.5f;
    u32 tag = 0; ///< stamped onto every edge, so the game can find its road type again

    float width() const { return static_cast<float>(lanes_forward + lanes_backward) * lane_width; }
    /// A junction has to be at least as big as the widest road meeting it,
    /// or the lanes would end outside the paved area.
    float junction_radius() const { return std::max(width() * 0.6f, 3.0f); }
};

/// Where a new road meets an existing one.
struct Crossing {
    float along = 0;      ///< metres along the new road
    u32 edge = kNoIndex;  ///< the existing road being crossed
    float on_edge = 0;    ///< metres along that road
    u32 node = kNoIndex;  ///< an existing junction, when the crossing lands on one
    Vec2 at{};
};

struct BuildResult {
    bool ok = false;
    const char* refusal = "";
    std::vector<u32> edges;     ///< the pieces of the new road, in order
    std::vector<u32> junctions; ///< every node it touches, new and reused
    float length = 0;
};

// ---- small helpers ------------------------------------------------------------------------

inline float path_length(const Path& p) { return p.length(); }

/// Nearest point on an edge's centreline, as a distance along it.
inline bool closest_on_edge(const CurveNetwork& net, u32 edge, Vec2 p, float max_distance, float& out_distance) {
    if (!net.valid_edge(edge)) return false;
    const PathSampler s(net.edge(edge).path, 0.25f);
    if (s.empty()) return false;
    const PathPoint hit = s.closest(p);
    if (rw::distance(hit.position, p) > max_distance) return false;
    out_distance = hit.distance;
    return true;
}

/// Every place the new road crosses an existing one, in order along the new
/// road. Crossings within `merge` metres of each other collapse into one, so
/// a road grazing a junction does not produce a shower of tiny edges.
inline std::vector<Crossing> find_crossings(const CurveNetwork& net, const Path& path, float snap, float merge = 4.0f) {
    std::vector<Crossing> found;
    std::vector<Vec2> line;
    path.flatten(0.25f, line);
    if (line.size() < 2) return found;
    const PathSampler sampler(path, 0.25f);

    std::vector<Vec2> other;
    for (u32 e = 0; e < static_cast<u32>(net.edges().size()); ++e) {
        if (!net.valid_edge(e)) continue;
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

/// Split an edge `distance` metres along, returning the new node. Keeps the
/// junction radius sensible and the node's own tag clean.
inline u32 split_at(CurveNetwork& net, u32 edge, float distance, float radius) {
    if (!net.valid_edge(edge)) return kNoIndex;
    const Path& path = net.edge(edge).path;
    const PathSampler s(path, 0.25f);
    const float len = s.length();
    if (len < 2 || distance < 1.0f || distance > len - 1.0f) return kNoIndex;
    const PathPoint at = s.at(distance);
    const u32 node = net.split_edge(edge, at.segment, at.t);
    if (node == kNoIndex) return kNoIndex;
    net.node(node).radius = std::max(net.node(node).radius, radius);
    net.node(node).tag = 0;
    return node;
}

/// Lay `path` into the network. Its ends snap to nearby junctions, or split
/// the road they land on; everything it crosses on the way is split too, and
/// the new road is added as one piece between each pair of junctions.
inline BuildResult build_road(CurveNetwork& net, const Path& path, const RoadSpec& spec, float snap,
                              float min_length = 12.0f) {
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

    const float radius = spec.junction_radius();
    const Vec2 start = path.start();
    const Vec2 end = path.end();

    // Ends first: an existing junction wins, otherwise a road to split,
    // otherwise a new junction of our own.
    struct End {
        u32 node = kNoIndex;
        u32 edge = kNoIndex;
        float on_edge = 0;
    };
    auto resolve = [&](Vec2 at) {
        End e;
        e.node = net.nearest_node(at, snap);
        if (e.node != kNoIndex) return e;
        const u32 edge = net.nearest_edge(at, 1e30f);
        float distance = 0;
        if (edge != kNoIndex && closest_on_edge(net, edge, at, snap, distance)) {
            e.edge = edge;
            e.on_edge = distance;
        }
        return e;
    };
    End head = resolve(start);
    End tail = resolve(end);
    if (head.node != kNoIndex && head.node == tail.node) {
        result.refusal = "starts and ends at the same junction";
        return result;
    }

    std::vector<Crossing> crossings = find_crossings(net, path, snap);
    // The ends are handled separately, so drop crossings that sit on them.
    crossings.erase(std::remove_if(crossings.begin(), crossings.end(),
                                   [&](const Crossing& c) { return c.along < snap || c.along > total - snap; }),
                    crossings.end());

    // Split from the far end of each road backwards, so the distances of the
    // crossings still to come stay on the piece that keeps its index.
    struct Split {
        u32 edge;
        float on_edge;
        usize slot; ///< index into `crossings`, or ~0 for an end
    };
    std::vector<Split> splits;
    for (usize i = 0; i < crossings.size(); ++i)
        if (crossings[i].node == kNoIndex) splits.push_back(Split{crossings[i].edge, crossings[i].on_edge, i});
    if (head.node == kNoIndex && head.edge != kNoIndex) splits.push_back(Split{head.edge, head.on_edge, ~usize(0)});
    if (tail.node == kNoIndex && tail.edge != kNoIndex) splits.push_back(Split{tail.edge, tail.on_edge, ~usize(0) - 1});
    std::sort(splits.begin(), splits.end(), [](const Split& a, const Split& b) {
        return a.edge != b.edge ? a.edge < b.edge : a.on_edge > b.on_edge;
    });
    for (const Split& s : splits) {
        const u32 node = split_at(net, s.edge, s.on_edge, radius);
        if (node == kNoIndex) continue;
        if (s.slot == ~usize(0)) head.node = node;
        else if (s.slot == ~usize(0) - 1) tail.node = node;
        else crossings[s.slot].node = node;
    }

    if (head.node == kNoIndex) head.node = net.add_node(start, 0, 0, radius);
    if (tail.node == kNoIndex) tail.node = net.add_node(end, 0, 0, radius);
    if (head.node == tail.node) {
        result.refusal = "starts and ends at the same junction";
        return result;
    }

    // Lay the road as one piece between each pair of junctions.
    std::vector<std::pair<float, u32>> stops;
    stops.emplace_back(0.0f, head.node);
    for (const Crossing& c : crossings)
        if (c.node != kNoIndex) stops.emplace_back(c.along, c.node);
    stops.emplace_back(total, tail.node);
    std::sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    for (usize i = 1; i < stops.size(); ++i) {
        const float from = stops[i - 1].first;
        const float to = stops[i].first;
        const u32 a = stops[i - 1].second;
        const u32 b = stops[i].second;
        if (a == b || to - from < 1.0f) continue;
        Path piece = path.sub_path(from, to);
        if (piece.empty()) continue;
        const u32 edge = net.add_edge(a, b, &piece);
        if (edge == kNoIndex) continue;
        net.set_tracks(edge, spec.lanes_forward, spec.lanes_backward, spec.lane_width);
        net.edge(edge).tag = spec.tag;
        result.edges.push_back(edge);
        if (result.junctions.empty()) result.junctions.push_back(a);
        result.junctions.push_back(b);
    }

    if (result.edges.empty()) {
        result.refusal = "nothing to connect";
        return result;
    }
    // Every junction the new road touches has to be big enough for it.
    for (u32 node : result.junctions)
        if (net.valid_node(node)) net.node(node).radius = std::max(net.node(node).radius, radius);
    net.rebuild();
    result.ok = true;
    return result;
}

/// Lay a roundabout: a ring of one-way arcs around `centre`. Traffic keeps
/// right, so the ring runs clockwise on screen (where y grows downwards),
/// which is anticlockwise on a map with north up.
///
/// The ring is laid with tight snapping so nothing already there pulls it out
/// of shape, then everything left inside it is cleared away: the roads that
/// crossed the ring are cut at the boundary and become its entries.
inline BuildResult build_roundabout(CurveNetwork& net, Vec2 centre, float radius, const RoadSpec& spec, float snap) {
    BuildResult result;
    if (radius < 6) {
        result.refusal = "too small";
        return result;
    }
    constexpr int kArcs = 8;
    constexpr float kStep = -6.2831853f / kArcs; // negative: clockwise on screen
    const float weld = std::min(snap, radius * 0.3f);

    std::vector<u32> ring;
    for (int i = 0; i < kArcs; ++i) {
        const float angle = static_cast<float>(i) * kStep;
        const Vec2 from{centre.x + std::cos(angle) * radius, centre.y + std::sin(angle) * radius};
        Path arc;
        arc.move_to(from);
        arc.arc_to(centre, kStep);
        const BuildResult piece = build_road(net, arc, spec, weld, 3.0f);
        for (u32 e : piece.edges) ring.push_back(e);
        for (u32 n : piece.junctions) result.junctions.push_back(n);
    }
    if (ring.empty()) {
        result.refusal = "no room for a roundabout here";
        return result;
    }

    // Clear the island: anything reaching inside the ring is a leftover of
    // whatever used to be here, and the entries stop at the boundary.
    const float inside = radius - 2.0f;
    const auto is_ring = [&](u32 edge) { return std::find(ring.begin(), ring.end(), edge) != ring.end(); };
    for (u32 e = 0; e < static_cast<u32>(net.edges().size()); ++e) {
        if (!net.valid_edge(e) || is_ring(e)) continue;
        const NetEdge& edge = net.edge(e);
        const bool a_in = rw::distance(net.node(edge.a).pos, centre) < inside;
        const bool b_in = rw::distance(net.node(edge.b).pos, centre) < inside;
        if (a_in || b_in) net.remove_edge(e);
    }
    for (u32 n = 0; n < static_cast<u32>(net.nodes().size()); ++n) {
        if (!net.valid_node(n)) continue;
        if (rw::distance(net.node(n).pos, centre) < inside && net.edges_at(n).empty()) net.remove_node(n);
    }

    result.edges = ring;
    result.ok = true;
    result.length = 6.2831853f * radius;
    net.rebuild();
    return result;
}

} // namespace traffic
