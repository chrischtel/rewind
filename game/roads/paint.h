#pragma once

// What a road network looks like from above: paving, kerbs, islands,
// hatched areas and every painted line, as meshes ready to draw.
//
// Nothing is laid out by hand. The paving is the union of what the lanes
// sweep (network_surface), and the paint follows from the same data:
//   - the edge line is the outline moved in a little, so it runs wherever
//     there is kerb, round corners and islands alike;
//   - where a lane joins or leaves alongside, the edge carries on broken;
//   - the centre line runs through a junction, solid where nothing crosses
//     it and broken where turning traffic does;
//   - a road that carries straight on has priority and no line across it;
//     a road that ends there gives way at the broken edge line of the road
//     it joins, which runs across its mouth;
//   - guide lines take lanes and left turns across the open middle.
// No line touches another: each is cut where an earlier one already lies.

#include "lanes.h"

#include <rewind/core/polygon.h>
#include <rewind/render/mesh.h>
#include <rewind/render/renderer2d.h>
#include <rewind/sim/network_surface.h>

#include <utility>

namespace roads {

using rw::Color;
using rw::Mesh;
using rw::NetworkSurface;
using rw::PolygonSet;
using rw::Ring;

constexpr float kPi = 3.14159265f;
constexpr float kLane = 3.5f;
constexpr float kKerbWidth = 0.3f;
constexpr float kLine = 0.12f;      ///< lane and edge lines, in metres
constexpr float kEdgeInset = 0.25f; ///< edge lines sit this far in from the kerb
constexpr float kClear = 0.8f;      ///< no line comes within half of this of another: paint never touches paint
constexpr u32 kNoLane = ~0u;
// Guide lines inside a junction, after the German marking rules (RMS): a
// narrow line in short dashes, much closer set than a lane line on the open
// road; the continued edge of the carriageway is a wide one.
constexpr float kGuide = kLine;
constexpr float kSolidAhead = 20.0f; ///< the lane line is solid this far before a junction where lanes are sorted
constexpr float kWarnAhead = 22.5f;  ///< and a warning line this long, five dashes, announces it
constexpr float kGuideDash = 1.0f;
constexpr float kWide = 0.25f;
constexpr float kShifted = 1.5f;    ///< arms offset by more than this lead their lanes across in guide dashes
constexpr float kFarAcross = 18.0f; ///< a turn longer than this crosses open paving and gets a guide
constexpr float kBeside = 0.7f;     ///< a border this close to a solid line is that line

constexpr Color kGrass = rw::rgb(124, 148, 92);
constexpr Color kIsland = rw::rgb(138, 164, 100);
constexpr Color kAsphalt = rw::rgb(58, 59, 63);
constexpr Color kKerb = rw::rgb(198, 194, 182);
constexpr Color kPaint = rw::rgb(238, 236, 226);

class RoadPaint {
public:
    Mesh ground; ///< islands, kerb, paving: draw first
    Mesh lanes;  ///< the swept bands themselves, translucent, for looking at how it works
    Mesh paint;  ///< every line and hatching: draw last
    NetworkSurface surface;
    /// Paint a separate give-way line (the thick one made of short blocks)
    /// in front of every approach that has to wait. Off by default: at an
    /// ordinary junction the broken edge line of the road being joined runs
    /// right across the mouth, and that is where one waits. The block line is
    /// an extra some junctions get; a solid stop line belongs to a stop sign
    /// or a signal, and there are none of those here yet.
    bool wait_lines = false;
    /// Junctions with traffic lights get a stop line across every approach;
    /// those where people cross as well get a crossing beyond it.
    std::vector<u32> stop_nodes, walk_nodes;
    /// In town a lane line is 3 m of paint and 3 m of gap; out of town the gap is 6 m.
    bool urban = true;
    float lane_gap() const { return urban ? 3.0f : 6.0f; }

    /// Work everything out again from the network.
    void rebuild(const CurveNetwork& net) {
        net_ = &net;
        ground.clear();
        lanes.clear();
        paint.clear();

        rw::SurfaceOptions options;
        options.corner_radius = 2.5f;
        options.min_keep_clear_width = 1.2f; // a wedge has to be worth hatching
        options.min_keep_clear_area = 3.0f;
        surface = rw::network_surface(net, options);
        NetworkSurface& s = surface;

        // Hatching keeps traffic out of the space between lanes that run
        // side by side. The very middle of a junction, where paths cross, is
        // just road: an unswept scrap there is left plain.
        PolygonSet marked;
        for (const rw::Polygon& area : s.keep_clear.polygons) {
            const Vec2 middle = rw::polygon_inner_circle(area, 0.1f).center;
            bool in_a_junction = false;
            for (u32 n = 0; n < net.nodes().size() && !in_a_junction; ++n)
                in_a_junction = net.valid_node(n) && net.edges_at(n).size() >= 3 &&
                                rw::distance(net.node(n).pos, middle) < net.node(n).radius * 0.5f;
            // Nor is a small scrap out in the open road, bounded by nothing
            // but other people's turns. What is hatched is either a real
            // area (the space a widened road leaves in its middle) or a nose
            // against a kerb, where a lane peels away round an island.
            PolygonSet one;
            one.polygons.push_back(area);
            const bool sizeable = one.area() >= 25.0f;
            const bool against_kerb = rw::polygon_difference(rw::polygon_offset(one, 0.7f), s.paved).area() > 0.3f;
            const bool a_nose = against_kerb && one.area() >= 6.0f; // less is the crumb in the crotch of a fork
            if (!in_a_junction && (sizeable || a_nose)) marked.polygons.push_back(area);
        }
        s.keep_clear = marked;

        // Everything further than an edge line's inset from the outline: a
        // lane edge in here has another lane beside it, not kerb.
        interior_ = rw::polygon_offset(s.paved, -(kEdgeInset + 0.2f));
        hatched_ = rw::polygon_offset(s.keep_clear, 0.4f);

        // From the bottom up, and none of it overlapping: the island grass,
        // the kerb as the ring around the paving (which also rings every
        // island, since an island is a hole in the paving), the paving.
        fill(s.islands, kIsland, ground);
        fill(rw::polygon_border(s.paved, kKerbWidth), kKerb, ground);
        fill(s.paved, kAsphalt, ground);
        fill(s.lanes, rw::rgb(96, 150, 220), lanes, 0.45f);

        // Keep-clear: stripes, set in from the edge.
        fill(rw::polygon_hatch(s.keep_clear, kPi * 0.25f, 1.3f, 0.4f, 0.5f), kPaint, paint);

        mouths_.clear();
        solid_.clear();
        mark_outline();
        for (u32 e = 0; e < net.edges().size(); ++e)
            if (net.valid_edge(e)) mark_edge(e);
        for (u32 n = 0; n < net.nodes().size(); ++n)
            if (net.valid_node(n) && net.edges_at(n).size() >= 2) mark_node(n);
        for (u32 n = 0; n < net.nodes().size(); ++n)
            if (net.valid_node(n)) mark_arrows(n);
        for (u32 n : stop_nodes)
            if (net.valid_node(n)) mark_lights(n, std::find(walk_nodes.begin(), walk_nodes.end(), n) != walk_nodes.end());

        // And the edge of each keep-clear area. Where a solid lane line runs
        // along it, that line is its edge: the centre line of a road that
        // widens parts in two, and the two halves carry on down either side
        // of the hatching without a break. Only where no such line runs (the
        // nose where a lane peels away, the end of a turn lane) does the area
        // get a border of its own.
        // That border is drawn exactly where a lane line along the same edge
        // would be, so one runs into the other without a step, and it stays
        // on the paving: against an island it is the island's edge line.
        const PolygonSet lined = rw::polygon_union(solid_);
        const PolygonSet on_paving = rw::polygon_offset(s.paved, -0.1f);
        for (Ring ring : rw::polygon_offset(s.keep_clear, kLine).rings()) {
            if (ring.size() < 3) continue;
            ring.push_back(ring.front());
            for (const auto& kept : rw::clip_polyline(ring, on_paving, true))
                for (const auto& piece : rw::clip_polyline(kept, lined, false))
                    if (length_of(piece) > 1.5f) line(piece, kLine);
        }
        net_ = nullptr;
    }

    static void fill(const PolygonSet& set, Color c, Mesh& out, float alpha = 1) {
        rw::build_polygon_set(set, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, alpha, out);
    }

private:
    const CurveNetwork& net() const { return *net_; }

    void line(const std::vector<Vec2>& points, float width, float dash = 0, float gap = 0) {
        if (points.size() < 2) return;
        rw::StrokeStyle s;
        s.width = width;
        s.dash = dash;
        s.gap = gap;
        s.r = kPaint.r / 255.0f;
        s.g = kPaint.g / 255.0f;
        s.b = kPaint.b / 255.0f;
        if (dash <= 0) {
            rw::build_polyline(points, s, paint);
            return;
        }
        // A broken line is made of whole dashes: as many as fit, centred, with
        // at least half a gap left at each end. So no line ends in a stub, and
        // where two broken lines meet end to end there is a full gap between
        // them. A stretch too short for one dash and its gap is left bare.
        const float total = length_of(points), period = dash + gap;
        const float count = std::floor(total / period);
        if (count < 1) return;
        const float spare = (total - count * period) * 0.5f + gap * 0.5f;
        const std::vector<Vec2> fitted = shortened(points, spare, spare);
        if (fitted.size() >= 2) rw::build_polyline(fitted, s, paint);
    }

    /// A lane line. A solid one runs its whole length and is remembered, so
    /// that a keep-clear area beside it takes it for its edge. A broken one
    /// stops at a keep-clear area, which has a solid edge there instead.
    void lane_line(const std::vector<Vec2>& points, float width, float dash = 0, float gap = 0) {
        if (dash <= 0) {
            if (points.size() < 2) return;
            line(points, width);
            solid_.push_back(rw::polyline_band(points, kBeside));
            return;
        }
        for (const auto& piece : rw::clip_polyline(points, hatched_, false))
            if (length_of(piece) > 0.5f) line(piece, width, dash, gap);
    }

    /// The edge line: one solid line just inside the kerb, wherever there is
    /// kerb. It is the outline of the paving moved in a little, so it runs
    /// unbroken along the far side of a T, round every corner into the side
    /// road, along a taper and round every island, without any of those
    /// being a case of its own.
    void mark_outline() {
        // Not across the end of a road that simply stops.
        std::vector<PolygonSet> skip;
        for (u32 n = 0; n < net().nodes().size(); ++n) {
            if (!net().valid_node(n)) continue;
            const std::vector<u32> edges = net().edges_at(n);
            if (edges.size() == 1)
                skip.push_back(PolygonSet::from_circle(net().node(n).pos, net().edge(edges[0]).width * 0.5f + 1.0f, 24));
        }
        const PolygonSet no_line = rw::polygon_union(skip);
        for (Ring ring : rw::polygon_offset(surface.paved, -kEdgeInset).rings()) {
            if (ring.size() < 3) continue;
            ring.push_back(ring.front()); // a ring is closed; a line has to say so
            for (const auto& piece : rw::clip_polyline(ring, no_line, false)) {
                if (length_of(piece) <= 1.0f) continue;
                line(piece, kLine);
                solid_.push_back(rw::polyline_band(piece, kBeside)); // a hatched nose against this kerb takes it for its edge
            }
        }
    }

    /// Lines along one road. A lane knows its own left and right; which line
    /// goes there depends on what lies beyond: kerb (the edge line, drawn
    /// with the outline), a lane the same way (dashes), or oncoming traffic
    /// (a solid line).
    void mark_edge(u32 e) {
        const NetEdge& edge = net().edge(e);
        for (u32 t = 0; t < edge.tracks.size(); ++t) {
            const Track& lane = edge.tracks[t];
            const TrackRef ref{e, t};
            const Track* left = beside(edge, lane, -1);
            const Track* right = beside(edge, lane, 1);
            if (!right) {
                // Kerb on the right, usually. Where there is none, because a
                // lane is joining or leaving alongside, the edge of the road
                // carries on as a broken line.
                for (const auto& open : rw::clip_polyline(rw::track_edge(net(), ref, 1, kEdgeInset), interior_, true)) {
                    if (length_of(open) < 5.0f) continue; // a corner that is merely generous is not a lane joining
                    lane_line(open, kWide, 1.0f, 1.0f);
                    mouths_ = rw::polygon_union(mouths_, rw::polyline_band(open, kClear));
                }
            } else if (right->forward == lane.forward) {
                // A lane the same way on the right: dashes wherever both
                // exist. Short ones beside a lane that opens or closes.
                // A lane that opens or closes alongside, for joining or
                // leaving, is set off by a wide line in short dashes.
                const bool plain = lane.whole() && right->whole();
                // Coming up to a junction where the lanes are sorted, the
                // line turns solid: whoever is in a lane stays in it. A
                // warning line, dashes close together, says so beforehand.
                u32 ahead = kNoLane;
                for (u32 node : {edge.a, edge.b})
                    if (net().arrives_at(ref, node)) ahead = node;
                const bool sorted = plain && ahead != kNoLane && net().edges_at(ahead).size() >= 3 && !priority_at(net(), ahead).equals;
                const Vec2 stop = net().track_path(ref).empty() ? Vec2{} : net().track_path(ref).end();
                for (const auto& piece : rw::clip_polyline(rw::track_edge(net(), ref, 1), interior_, true)) {
                    const float length = length_of(piece);
                    if (!plain) {
                        lane_line(piece, kWide, 1.5f, 1.5f);
                    } else if (!sorted || length < 40.0f || rw::distance(piece.back(), stop) > kLane * 2) {
                        lane_line(piece, kLine, 3.0f, lane_gap());
                    } else {
                        const float solid = std::min(kSolidAhead, length * 0.25f), warn = std::min(kWarnAhead, length * 0.25f);
                        lane_line(shortened(piece, 0, solid + warn), kLine, 3.0f, lane_gap());
                        lane_line(shortened(piece, length - solid - warn, solid), kLine, 3.0f, 1.5f);
                        lane_line(shortened(piece, length - solid, 0), kLine);
                    }
                }
            }
            // Oncoming traffic on the left: each direction draws its own
            // line, which makes the double line down the middle.
            if (left && left->forward != lane.forward) lane_line(rw::track_edge(net(), ref, -1, kLine), kLine);
        }
    }

    /// Arrows on the lanes coming up to a junction: one in each lane, showing
    /// the turns it makes, a few metres before the lane ends, and once more
    /// further back on a long road. Only where there is something to say: a
    /// road that brings several lanes, or a lane the player has given arrows.
    void mark_arrows(u32 node) {
        if (net().edges_at(node).size() < 3) return;
        std::vector<Ring> shapes;
        for (u32 e : net().edges_at(node)) {
            const NetEdge& edge = net().edge(e);
            u32 arriving = 0;
            bool chosen = false;
            for (u32 t = 0; t < edge.tracks.size(); ++t) {
                if (!net().arrives_at(TrackRef{e, t}, node)) continue;
                ++arriving;
                chosen = chosen || edge.tracks[t].tag != kAny;
            }
            if (arriving < 2 && !chosen) continue;
            for (u32 t = 0; t < edge.tracks.size(); ++t) {
                const TrackRef ref{e, t};
                if (!net().arrives_at(ref, node)) continue;
                const u32 arrows = arrows_of(net(), ref);
                if (arrows == 0) continue;
                const std::vector<Vec2> centre = rw::track_edge(net(), ref, 0);
                const float length = length_of(centre);
                for (float back : {12.0f, 34.0f}) {
                    if (length < back + (back > 20 ? 30.0f : 4.0f)) continue;
                    arrow_at(centre, length - back, arrows, shapes);
                }
            }
        }
        if (!shapes.empty()) fill(rw::polygon_union(shapes), kPaint, paint);
    }

    /// One arrow: a stem, and a head for each way the lane goes.
    static void arrow_at(const std::vector<Vec2>& centre, float along, u32 arrows, std::vector<Ring>& out) {
        Vec2 at{}, dir{1, 0};
        float walked = 0;
        for (usize i = 0; i + 1 < centre.size(); ++i) {
            const float len = rw::distance(centre[i], centre[i + 1]);
            if (len > 1e-5f && (walked + len >= along || i + 2 == centre.size())) {
                dir = (centre[i + 1] - centre[i]) * (1.0f / len);
                at = centre[i] + dir * (along - walked);
                break;
            }
            walked += len;
        }
        const Vec2 right{-dir.y, dir.x};
        const auto p = [&](float u, float v) { return at + right * u + dir * v; };
        const bool straight = (arrows & kStraight) != 0;
        const float branch = straight ? 2.3f : 2.85f, top = straight ? 3.4f : 3.0f;
        out.push_back({p(-0.15f, 0), p(0.15f, 0), p(0.15f, top), p(-0.15f, top)});
        if (straight) out.push_back({p(-0.55f, 3.4f), p(0.55f, 3.4f), p(0, 5.0f)});
        for (float side : {-1.0f, 1.0f}) {
            if ((arrows & (side > 0 ? kRight : kLeft)) == 0) continue;
            out.push_back({p(0, branch - 0.15f), p(0.65f * side, branch - 0.15f), p(0.65f * side, branch + 0.15f), p(0, branch + 0.15f)});
            out.push_back({p(0.6f * side, branch - 0.55f), p(1.4f * side, branch), p(0.6f * side, branch + 0.55f)});
        }
    }

    /// What traffic lights add to a junction: the stop line, a solid bar
    /// half a metre wide across the lanes that arrive, and, where people
    /// cross, the crossing beyond it as two broken lines across the road.
    void mark_lights(u32 node, bool crossing) {
        for (u32 e : net().edges_at(node)) {
            const NetEdge& edge = net().edge(e);
            Vec2 on{};          // towards the junction
            Vec2 base{};        // a point on the line where the lanes end
            bool found = false;
            std::vector<Vec2> ends;
            for (u32 t = 0; t < edge.tracks.size(); ++t) {
                const TrackRef ref{e, t};
                const Path& path = net().track_path(ref);
                if (path.empty()) continue;
                const std::vector<Vec2> lhs = rw::track_edge(net(), ref, -1), rhs = rw::track_edge(net(), ref, 1);
                if (lhs.size() < 2 || rhs.size() < 2) continue;
                if (net().arrives_at(ref, node)) {
                    const Vec2 dir = path.segments.back().direction(1);
                    line({lhs.back() - dir * 0.3f, rhs.back() - dir * 0.3f}, 0.5f);
                    on = dir;
                    base = path.end();
                    found = true;
                    ends.push_back(lhs.back());
                    ends.push_back(rhs.back());
                } else if (net().leaves_from(ref, node)) {
                    ends.push_back(lhs.front());
                    ends.push_back(rhs.front());
                }
            }
            if (!crossing || !found || ends.size() < 2) continue;
            const Vec2 across{-on.y, on.x};
            float lo = 1e30f, hi = -1e30f;
            for (Vec2 p : ends) {
                lo = std::min(lo, rw::dot(p - base, across));
                hi = std::max(hi, rw::dot(p - base, across));
            }
            for (float ahead : {0.9f, 3.9f})
                line({base + on * ahead + across * lo, base + on * ahead + across * hi}, kLine, 0.5f, 0.5f);
        }
    }

    /// Everything painted inside one junction (or across one seam in a road).
    void mark_node(u32 node) {
        const usize arms = net().edges_at(node).size();
        // More than four roads meeting in one open space is no place for
        // guide lines: every one of them would cross several others. Such a
        // junction keeps its edge lines and its give-way lines and is
        // otherwise left plain, as they are on the road.
        const std::vector<u32>& here = net().connectors_at(node);
        if (here.empty()) return;
        // Nor is a crossing of equals: where two roads of the same size
        // cross, neither has priority, nobody is told to wait, and the middle
        // is left clear. Where a bigger road crosses a smaller one, the
        // bigger one has priority and keeps its lines right through.
        const Priority priority = priority_at(net(), node);
        const bool equals = priority.equals;
        const bool plain = arms > 4 || equals;

        // Only what lies around this junction matters to it.
        const float reach = net().node(node).radius * 2 + 30;
        const Vec2 at = net().node(node).pos;
        const PolygonSet around = PolygonSet::from_rect({at.x - reach, at.y - reach, reach * 2, reach * 2});
        const PolygonSet local_mouths = rw::polygon_intersection(mouths_, around);

        // `painted` is everything a later line has to leave a gap for;
        // `ends` is what a turn guide stops at rather than crosses: the
        // centre line it sets out from and the edge line it arrives at.
        PolygonSet painted = rw::polygon_union(rw::polygon_intersection(hatched_, around), local_mouths);
        PolygonSet ends = local_mouths;
        const auto guide = [&](const std::vector<Vec2>& points, float dash, float gap) {
            if (points.size() < 2) return;
            // A guide that earlier paint would chop to pieces guides nobody: all or nothing.
            const auto pieces = rw::clip_polyline(points, painted, false);
            float left = 0;
            for (const auto& piece : pieces) left += length_of(piece);
            if (left < length_of(points) * 0.7f || pieces.size() > 2) return;
            for (const auto& piece : pieces)
                if (length_of(piece) > dash) line(piece, dash < 2.0f ? kGuide : kLine, dash, gap);
            painted = rw::polygon_union(painted, rw::polyline_band(points, kClear)); // nothing else within reach of it
        };
        const auto from_lane = [&](const rw::Connector& k) -> const Track& { return net().edge(k.from.edge).tracks[k.from.track]; };
        const auto to_lane = [&](const rw::Connector& k) -> const Track& { return net().edge(k.to.edge).tracks[k.to.track]; };
        /// Does this arm belong to a road with priority here?
        const auto carries_on = [&](u32 edge) { return priority.has(edge); };
        /// Does this movement follow the priority road through?
        const auto along = [&](const rw::Connector& k) { return priority.along(net(), k); };

        // The centre line of a road that carries straight on does not stop at
        // a junction, nor where the road is widened or narrowed. Where
        // nothing crosses it, it runs through solid, as along the road;
        // where turning traffic has to cross it, it runs through broken.
        std::vector<u32> centred; // the lower of the two roads, once the broken line is drawn
        if (!plain)
            for (u32 c : here) {
                const rw::Connector& k = net().connectors()[c];
                if (!along(k)) continue;
                // Oncoming traffic on its left, before the junction and after it.
                const Track* before = beside(net().edge(k.from.edge), from_lane(k), -1);
                const Track* after = beside(net().edge(k.to.edge), to_lane(k), -1);
                if (!before || !after || before->forward == from_lane(k).forward || after->forward == to_lane(k).forward) continue;

                const std::vector<Vec2> centre = rw::connector_edge(net(), c, -1);
                bool crossed = false;
                for (u32 other : here) {
                    const rw::Connector& o = net().connectors()[other];
                    if (other == c || (o.from.edge == k.to.edge && o.to.edge == k.from.edge)) continue; // itself, or oncoming
                    if (o.from.edge == k.from.edge && along(o)) continue;                                 // its neighbours, alongside
                    std::vector<Vec2> path;
                    o.path.flatten(0.1f, path);
                    if (crossing(path, centre)) {
                        crossed = true;
                        break;
                    }
                }
                if (!crossed) {
                    // Each direction draws its own, as along the road: a double line.
                    const std::vector<Vec2> own = rw::connector_edge(net(), c, -1, kLine);
                    lane_line(own, kLine);
                    painted = rw::polygon_union(painted, rw::polyline_band(own, kClear));
                    ends = rw::polygon_union(ends, rw::polyline_band(own, kClear));
                    continue;
                }
                const u32 key = std::min(k.from.edge, k.to.edge);
                if (std::find(centred.begin(), centred.end(), key) != centred.end()) continue; // the other direction drew it
                centred.push_back(key);
                // Broken across the junction, wherever it is not already the
                // edge of a hatched area. Round a corner, or across to an arm
                // that does not line up, it is a guide line: it shows where
                // the road goes, which there is not obvious.
                const bool led = priority.bends || shift_of(net(), k) > kShifted;
                for (const auto& piece : rw::clip_polyline(centre, painted, false)) {
                    if (led) line(piece, kGuide, kGuideDash * 1.5f, kGuideDash * 1.5f);
                    else line(piece, kLine, 3.0f, 1.5f);
                }
                painted = rw::polygon_union(painted, rw::polyline_band(centre, kClear));
                ends = rw::polygon_union(ends, rw::polyline_band(centre, kClear));
            }

        // Lanes going straight on side by side keep the line between them.
        if (!plain)
            for (u32 c : here) {
                const rw::Connector& k = net().connectors()[c];
                if (!along(k)) continue;
                const NetEdge& in = net().edge(k.from.edge);
                const Track* right = beside(in, from_lane(k), 1);
                if (!right || right->forward != from_lane(k).forward) continue;
                const TrackRef neighbour{k.from.edge, static_cast<u32>(right - in.tracks.data())};
                for (u32 other : net().connectors_from(neighbour)) {
                    const rw::Connector& o = net().connectors()[other];
                    if (o.node != node || o.to.edge != k.to.edge || !along(o)) continue;
                    if (o.to == k.to) continue; // two lanes becoming one: there is no lane left to divide
                    // The same line as along the road, unless the road shifts
                    // or bends on the way across: then it leads.
                    if (priority.bends || shift_of(net(), k) > kShifted) guide(rw::connector_edge(net(), c, 1), kGuideDash, kGuideDash);
                    else guide(rw::connector_edge(net(), c, 1), 3.0f, lane_gap());
                    break;
                }
            }

        // Two lanes that make the same turn side by side always get a guide
        // between them, on any junction: it keeps the inner one from drifting
        // out and the outer one from cutting in.
        for (u32 c : here) {
            const rw::Connector& k = net().connectors()[c];
            const int turn = turn_of(net(), k.from, k.to);
            if (turn == 0 || turn == kBack || along(k)) continue;
            const NetEdge& in = net().edge(k.from.edge);
            const Track* right = beside(in, from_lane(k), 1);
            if (!right || right->forward != from_lane(k).forward) continue;
            const TrackRef neighbour{k.from.edge, static_cast<u32>(right - in.tracks.data())};
            for (u32 other : net().connectors_from(neighbour)) {
                const rw::Connector& o = net().connectors()[other];
                if (o.node != node || o.to.edge != k.to.edge || o.to == k.to) continue;
                const std::vector<Vec2> between = shortened(rw::connector_edge(net(), c, 1), 1.0f, 1.0f);
                for (const auto& piece : rw::clip_polyline(between, painted, false))
                    if (length_of(piece) > kGuideDash) line(piece, kGuide, kGuideDash, kGuideDash);
                painted = rw::polygon_union(painted, rw::polyline_band(between, kClear));
                break;
            }
        }
        if (arms < 3) return;

        // The edge of the road carries on across the mouth of a side road, a
        // slip lane joining or a roundabout entry as a broken line: the kerb
        // lane's outer edge, wherever it has no kerb beside it.
        PolygonSet mouths = local_mouths;
        std::vector<std::vector<Vec2>> mouth_lines;
        if (!plain)
            for (u32 c : here) {
                const rw::Connector& k = net().connectors()[c];
                if (!along(k)) continue;
                // Only the outermost lane that carries on; a turn pocket beside it does not count.
                const Track* outer = beside(net().edge(k.from.edge), from_lane(k), 1);
                if (outer && outer->forward == from_lane(k).forward && outer->tag == from_lane(k).tag) continue;
                for (const auto& open : rw::clip_polyline(rw::connector_edge(net(), c, 1, kEdgeInset), interior_, true)) {
                    if (length_of(open) < 2.0f) continue;
                    mouths = rw::polygon_union(mouths, rw::polyline_band(open, kClear));
                    mouth_lines.push_back(open);
                }
            }
        ends = rw::polygon_union(ends, mouths);


        // Who waits? There are no signals here, so nobody gets a stop line.
        // A road that carries straight on through the junction has priority
        // and no line at all: its traffic just drives. A road that ends
        // there gives way, and its give-way line lies where the waiting is
        // done: along the edge of the road being joined.
        for (u32 e : net().edges_at(node)) {
            const NetEdge& edge = net().edge(e);
            if (!wait_lines || equals || carries_on(e)) continue;
            // One line for the whole approach: every lane that arrives here,
            // from the centre line to the kerb, waits at the same place.
            u32 inner = kNoLane, outer = kNoLane;
            bool yields = false;
            for (u32 t = 0; t < edge.tracks.size(); ++t) {
                const TrackRef ref{e, t};
                if (!net().arrives_at(ref, node) || net().connectors_from(ref).empty()) continue;
                for (u32 c : net().connectors_from(ref)) {
                    const rw::Connector& k = net().connectors()[c];
                    yields = yields || !net().conflicts_of(c).empty() || net().connectors_into(k.to).size() > 1;
                }
                // How far towards the kerb a lane lies, whichever way its edge runs.
                const auto out = [&](u32 lane) { return edge.tracks[lane].offset * (edge.tracks[lane].forward ? 1.0f : -1.0f); };
                if (inner == kNoLane || out(t) < out(inner)) inner = t;
                if (outer == kNoLane || out(t) > out(outer)) outer = t;
            }
            if (!yields || inner == kNoLane) continue;
            const std::vector<Vec2> lhs = rw::track_edge(net(), TrackRef{e, inner}, -1);
            const std::vector<Vec2> rhs = rw::track_edge(net(), TrackRef{e, outer}, 1);
            if (lhs.size() < 2 || rhs.size() < 2) continue;
            const Vec2 on = rw::normalize(lhs.back() - lhs[lhs.size() - 2]);
            const Vec2 stop = (lhs.back() + rhs.back()) * 0.5f;
            // Straight ahead of the approach, the first edge line it comes
            // to: the give-way line goes just outside that, as wide as the
            // approach itself.
            const PolygonSet throat = rw::polyline_band({stop, stop + on * (net().node(node).radius * 2 + 4)},
                                                        rw::distance(lhs.back(), rhs.back()));
            std::vector<Vec2> nearest;
            for (const auto& mouth : mouth_lines)
                for (auto& piece : rw::clip_polyline(rw::offset_polyline(mouth, 0.55f, false), throat, true)) {
                    if (length_of(piece) < 1.5f) continue;
                    if (nearest.empty() || rw::distance(piece.front(), stop) < rw::distance(nearest.front(), stop))
                        nearest = std::move(piece);
                }
            if (!nearest.empty()) {
                line(nearest, kWide, 0.5f, 0.25f);
                painted = rw::polygon_union(painted, rw::polyline_band(nearest, 2.4f));
                continue;
            }
            // No edge line to wait at: straight across the end of the approach.
            line({lhs.back() - on * 0.25f, rhs.back() - on * 0.25f}, kWide, 0.5f, 0.25f);
        }
        if (plain) return;

        // Now the broken edge lines themselves, left out wherever somebody
        // waits: there the give-way line is the edge, and one broken line
        // on top of another is only noise.
        for (const auto& open : mouth_lines)
            for (const auto& piece : rw::clip_polyline(open, painted, false))
                if (length_of(piece) > 2.0f) line(piece, kWide, 1.0f, 1.0f);

        painted = rw::polygon_union(painted, mouths);

        // Then the left turns, which have the furthest to go without a kerb.
        // The busier road's turns come first and so are the ones that stay
        // whole where two would cross; among equals, the longer way round.
        std::vector<u32> lefts;
        for (u32 c : here)
            if (turn_of(net(), net().connectors()[c].from, net().connectors()[c].to) == -1) lefts.push_back(c);
        const auto lane_count = [&](u32 c) { return lane_of(net().edge(net().connectors()[c].from.edge), net().connectors()[c].from.track).count; };
        std::stable_sort(lefts.begin(), lefts.end(), [&](u32 x, u32 y) {
            if (lane_count(x) != lane_count(y)) return lane_count(x) > lane_count(y);
            return net().connectors()[x].length > net().connectors()[y].length;
        });
        // A turn guide belongs on the carriageway being crossed, between its
        // centre line and its edge line, and stops at both. Only the turn
        // off the priority road gets one, from the centre line out: that is
        // the turn made across several lanes of oncoming traffic. The turn
        // out of the side road would meet it at the mouth and make a fan of
        // dots there. Lane dividers crossed on the way get a gap. A stub is
        // left out, and so are two guides that would cross each other, as
        // the left turns of a road do when they face one another.
        std::vector<std::vector<Vec2>> guides;
        for (u32 c : lefts) {
            if (priority.bends) break; // the bend has its own guide lines, and one fan of them is enough
            const rw::Connector& k = net().connectors()[c];
            // On a plain two-lane road a turn has one lane to cross and needs
            // no help. A guide is painted where there are several: that is
            // where a driver can end up in the wrong one.
            // The same goes for a turn across a lot of open paving.
            if (!carries_on(k.from.edge) || along(k) || (lane_count(c) < 2 && k.length < kFarAcross)) continue;
            const std::vector<Vec2> whole = shortened(rw::connector_edge(net(), c, -1), 1.0f, 1.0f);
            const auto stretches = rw::clip_polyline(whole, ends, false);
            if (stretches.empty() || length_of(stretches[0]) < 2.5f) continue;
            guides.push_back(stretches[0]);
        }
        for (usize g = 0; g < guides.size(); ++g) {
            const std::vector<Vec2>& best = guides[g];
            bool crossed = false;
            for (usize o = 0; o < guides.size(); ++o) crossed = crossed || (o != g && crossing(best, guides[o]));
            if (crossed) continue;
            const auto pieces = rw::clip_polyline(best, painted, false);
            float left = 0;
            for (const auto& piece : pieces) left += length_of(piece);
            if (left < length_of(best) * 0.6f || pieces.size() > 3) continue;
            // Short dots rather than dashes: each stretch between the lines it
            // crosses is only a lane wide, and has to read as part of one arc.
            for (const auto& piece : pieces) line(piece, kGuide, kGuideDash, kGuideDash);
            painted = rw::polygon_union(painted, rw::polyline_band(best, kClear));
        }
    }

    const CurveNetwork* net_ = nullptr;
    PolygonSet interior_;
    PolygonSet hatched_; ///< keep-clear areas and a margin: no lane line is drawn in here
    PolygonSet mouths_;  ///< where broken edge lines run: turn guides end there
    std::vector<PolygonSet> solid_; ///< a narrow band along every solid lane line drawn
};

} // namespace roads
