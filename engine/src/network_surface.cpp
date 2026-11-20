#include "rewind/sim/network_surface.h"

#include <algorithm>
#include <cmath>

namespace rw {

namespace {

std::vector<Vec2> flatten(const Path& path, float tolerance) {
    std::vector<Vec2> out;
    path.flatten(std::max(tolerance, 0.005f), out);
    return out;
}

/// Push both ends of a line outwards along itself.
void lengthen(std::vector<Vec2>& line, float by) {
    if (line.size() < 2 || by <= 0) return;
    const Vec2 head = normalize(line[0] - line[1]);
    const Vec2 tail = normalize(line[line.size() - 1] - line[line.size() - 2]);
    line.front() = line.front() + head * by;
    line.back() = line.back() + tail * by;
}

/// A line beside `line`, at `d0` to its right at the start and `d1` at the
/// end, changing evenly along the length in between.
std::vector<Vec2> offset_tapered(const std::vector<Vec2>& line, float d0, float d1) {
    std::vector<Vec2> out;
    if (line.size() < 2) return out;
    float total = 0;
    for (usize i = 0; i + 1 < line.size(); ++i) total += distance(line[i], line[i + 1]);
    if (total <= 1e-6f) return out;
    out.reserve(line.size());
    float walked = 0;
    for (usize i = 0; i < line.size(); ++i) {
        if (i > 0) walked += distance(line[i - 1], line[i]);
        // The direction at a point is the average of the pieces either side.
        Vec2 dir{};
        if (i > 0) dir = dir + normalize(line[i] - line[i - 1]);
        if (i + 1 < line.size()) dir = dir + normalize(line[i + 1] - line[i]);
        if (length_sq(dir) < 1e-10f) dir = i + 1 < line.size() ? line[i + 1] - line[i] : line[i] - line[i - 1];
        dir = normalize(dir);
        out.push_back(line[i] + perp(dir) * lerpf(d0, d1, walked / total));
    }
    return out;
}

/// The area a line sweeps when its width changes from one end to the other.
PolygonSet band(const std::vector<Vec2>& line, float w0, float w1, float tolerance) {
    if (line.size() < 2) return {};
    if (absf(w0 - w1) < 1e-3f) {
        BandOptions o;
        o.arc_tolerance = std::max(tolerance, 0.005f);
        return polyline_band(line, w0, o);
    }
    Ring ring = offset_tapered(line, -w0 * 0.5f, -w1 * 0.5f);
    const std::vector<Vec2> right = offset_tapered(line, w0 * 0.5f, w1 * 0.5f);
    ring.insert(ring.end(), right.rbegin(), right.rend());
    return polygon_union(std::vector<Ring>{ring}); // a tight bend may fold the ring over itself
}

bool reaches(const std::vector<Vec2>& line, float margin, const Rect* region) {
    if (!region) return true;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (const Vec2& p : line) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    return x1 + margin >= region->x && x0 - margin <= region->x + region->w && y1 + margin >= region->y &&
           y0 - margin <= region->y + region->h;
}

float track_width(const CurveNetwork& net, TrackRef t) {
    if (!net.valid_edge(t.edge) || t.track >= net.edge(t.edge).tracks.size()) return 0;
    return net.edge(t.edge).tracks[t.track].width;
}

bool wanted(const CurveNetwork& net, const SurfaceOptions& o, TrackRef t) { return !o.include || o.include(net, t); }

/// A line beside `line`, each point moved to its right by its own amount.
std::vector<Vec2> offset_each(const std::vector<Vec2>& line, const std::vector<float>& by) {
    std::vector<Vec2> out;
    out.reserve(line.size());
    for (usize i = 0; i < line.size(); ++i) {
        Vec2 dir{};
        if (i > 0) dir = dir + normalize(line[i] - line[i - 1]);
        if (i + 1 < line.size()) dir = dir + normalize(line[i + 1] - line[i]);
        if (length_sq(dir) < 1e-10f) dir = i + 1 < line.size() ? line[i + 1] - line[i] : line[i] - line[i - 1];
        out.push_back(line[i] + perp(normalize(dir)) * by[i]);
    }
    return out;
}

/// Extra points every `step` along the first and last `zone` of a line, so a
/// width that changes there has something to change along.
std::vector<Vec2> refine_ends(const std::vector<Vec2>& line, float zone, float step, bool head, bool tail) {
    float total = 0;
    for (usize i = 0; i + 1 < line.size(); ++i) total += distance(line[i], line[i + 1]);
    std::vector<Vec2> out;
    out.push_back(line[0]);
    float walked = 0;
    for (usize i = 0; i + 1 < line.size(); ++i) {
        const float len = distance(line[i], line[i + 1]);
        if (len > 1e-6f) {
            // Cut this piece wherever a step falls inside one of the zones.
            float at = std::floor(walked / step) * step + step;
            for (; at < walked + len - 1e-4f; at += step) {
                const bool in_zone = (head && at < zone + step) || (tail && at > total - zone - step);
                if (in_zone) out.push_back(lerp(line[i], line[i + 1], (at - walked) / len));
            }
        }
        walked += len;
        out.push_back(line[i + 1]);
    }
    return out;
}

/// The two sides of a track, in its direction of travel: `left` and `right`
/// run beside its centre line at half its width, except where the track
/// starts or stops short of a node. There it opens over its taper from
/// nothing to full width, keeping straight the side that has a neighbouring
/// lane and swinging the other one out: the way a turn pocket is set out.
struct Sides {
    std::vector<Vec2> centre, left, right;
    bool tapered = false;
};

Sides track_sides(const CurveNetwork& net, TrackRef ref, float tolerance, float extra, float inset = 0) {
    Sides s;
    if (!net.valid_edge(ref.edge) || ref.track >= net.edge(ref.edge).tracks.size()) return s;
    const NetEdge& edge = net.edge(ref.edge);
    const Track& t = edge.tracks[ref.track];
    s.centre = flatten(net.track_path(ref), tolerance);
    if (s.centre.size() < 2) return s;
    const float half = std::max((t.width + extra) * 0.5f - inset, 0.0f);

    // Open ends, as travelled: a forward track starts at `begin` and stops at `end`.
    const bool open_head = (t.forward ? t.begin : t.end) > 0, open_tail = (t.forward ? t.end : t.begin) > 0;
    float total = 0;
    for (usize i = 0; i + 1 < s.centre.size(); ++i) total += distance(s.centre[i], s.centre[i + 1]);
    const float taper = std::min(t.taper, (open_head && open_tail) ? total * 0.5f : total);
    if (taper <= 1e-3f || (!open_head && !open_tail)) {
        const std::vector<float> l(s.centre.size(), -half), r(s.centre.size(), half);
        s.left = offset_each(s.centre, l);
        s.right = offset_each(s.centre, r);
        return s;
    }

    // Which side has a lane right beside it? Offsets are across the edge;
    // a track running against the edge sees them mirrored.
    // A lane going the same way counts for more than oncoming traffic: a
    // turn lane in the middle of the road opens out of the lane its traffic
    // comes from, not out of the centre line.
    int anchor = 0; // -1: the left side stays put, +1: the right, 0: both close in
    bool on_left = false, on_right = false, same_left = false, same_right = false;
    for (u32 j = 0; j < edge.tracks.size(); ++j) {
        if (j == ref.track) continue;
        const Track& o = edge.tracks[j];
        const float gap = o.offset - t.offset;
        if (absf(absf(gap) - (o.width + t.width) * 0.5f) > 0.3f) continue; // not touching
        const bool right = (gap > 0) == t.forward;
        (right ? on_right : on_left) = true;
        if (o.forward == t.forward) (right ? same_right : same_left) = true;
    }
    if (same_left != same_right) anchor = same_left ? -1 : 1;
    else if (on_left != on_right) anchor = on_left ? -1 : 1;

    s.tapered = true;
    s.centre = refine_ends(s.centre, taper, std::max(taper / 10.0f, 0.25f), open_head, open_tail);
    std::vector<float> l(s.centre.size()), r(s.centre.size());
    float walked = 0;
    for (usize i = 0; i < s.centre.size(); ++i) {
        if (i > 0) walked += distance(s.centre[i - 1], s.centre[i]);
        float open = 1;
        if (open_head) open = std::min(open, clampf(walked / taper, 0, 1));
        if (open_tail) open = std::min(open, clampf((total - walked) / taper, 0, 1));
        open = open * open * (3 - 2 * open); // an S, not a wedge: no kink where it starts or ends
        l[i] = anchor == -1 ? -half : anchor == 1 ? half - 2 * half * open : -half * open;
        r[i] = anchor == 1 ? half : anchor == -1 ? -half + 2 * half * open : half * open;
    }
    s.left = offset_each(s.centre, l);
    s.right = offset_each(s.centre, r);
    return s;
}

/// What one track sweeps: a plain band, or its tapered outline.
PolygonSet track_band(const CurveNetwork& net, TrackRef ref, const SurfaceOptions& o, const Rect* region) {
    const float extra = o.seam + o.widen;
    const float width = track_width(net, ref) + extra;
    if (width <= 0) return {};
    const Track& t = net.edge(ref.edge).tracks[ref.track];
    if (t.whole() || t.taper <= 0) {
        const std::vector<Vec2> line = flatten(net.track_path(ref), o.tolerance);
        if (line.size() < 2 || !reaches(line, width, region)) return {};
        return band(line, width, width, o.tolerance);
    }
    const Sides sides = track_sides(net, ref, o.tolerance, extra);
    if (sides.left.size() < 2 || !reaches(sides.centre, width, region)) return {};
    Ring ring = sides.left;
    ring.insert(ring.end(), sides.right.rbegin(), sides.right.rend());
    return polygon_union(std::vector<Ring>{ring});
}

void sweep_connector(const CurveNetwork& net, const Connector& k, const SurfaceOptions& o, const Rect* region,
                     std::vector<PolygonSet>& bands) {
    if (!wanted(net, o, k.from) || !wanted(net, o, k.to)) return;
    std::vector<Vec2> line = flatten(k.path, o.tolerance);
    if (line.size() < 2) return;
    const float extra = o.seam + o.widen;
    const float w0 = track_width(net, k.from) + extra, w1 = track_width(net, k.to) + extra;
    if (!reaches(line, std::max(w0, w1), region)) return;
    // A hair past both ends, so it laps over the tracks it joins.
    lengthen(line, o.seam);
    PolygonSet b = band(line, w0, w1, o.tolerance);
    if (!b.empty()) bands.push_back(std::move(b));
}

/// Everything after the sweeping: close up, sort the gaps, find what is unswept.
NetworkSurface finish(const std::vector<PolygonSet>& bands, const SurfaceOptions& o) {
    NetworkSurface s;
    if (bands.empty()) return s;
    const float arc = std::max(o.tolerance, 0.005f);
    s.lanes = polygon_union(bands);
    // Closing rounds with short chords, which shaves a sliver off every sharp
    // outward corner (the square end of a road); uniting with the lanes again
    // puts those back, so the paving covers every lane exactly.
    const PolygonSet closed = polygon_union(polygon_close(s.lanes, o.corner_radius, arc), s.lanes);

    // What the paving encloses without covering: an island when there is
    // room for one, otherwise just more paving.
    PolygonSet filled;
    for (const Polygon& gap : polygon_holes(closed).polygons) {
        PolygonSet one;
        one.polygons.push_back(gap);
        const bool island = one.area() >= o.min_island_area &&
                            polygon_inner_circle(gap, 0.05f).radius * 2.0f >= o.min_island_width;
        (island ? s.islands : filled).polygons.push_back(gap);
    }
    s.paved = filled.empty() ? closed : polygon_union(closed, filled);

    // Paved, but on nobody's path. Opening by half the minimum width drops
    // the slivers and the crescents in rounded corners and keeps the wedges.
    PolygonSet unswept = polygon_difference(s.paved, s.lanes);
    if (o.min_keep_clear_width > 0) unswept = polygon_open(unswept, o.min_keep_clear_width * 0.5f, arc);
    s.keep_clear = polygon_remove_small(unswept, o.min_keep_clear_area);
    return s;
}

} // namespace

NetworkSurface network_surface(const CurveNetwork& net, const SurfaceOptions& options, const Rect* region) {
    std::vector<PolygonSet> bands;
    for (u32 i = 0; i < net.track_count(); ++i) {
        const TrackRef ref = net.track_ref(i);
        if (!wanted(net, options, ref)) continue;
        PolygonSet b = track_band(net, ref, options, region);
        if (!b.empty()) bands.push_back(std::move(b));
    }
    for (const Connector& k : net.connectors()) sweep_connector(net, k, options, region, bands);
    return finish(bands, options);
}

NetworkSurface node_surface(const CurveNetwork& net, u32 node, float reach, const SurfaceOptions& options) {
    std::vector<PolygonSet> bands;
    if (!net.valid_node(node)) return {};
    const float extra = options.seam + options.widen;
    for (u32 e : net.edges_at(node)) {
        const NetEdge& edge = net.edge(e);
        for (u32 t = 0; t < edge.tracks.size(); ++t) {
            const TrackRef ref{e, t};
            if (!wanted(net, options, ref)) continue;
            const Path& path = net.track_path(ref);
            if (path.empty()) continue;
            const float len = path.length();
            // A road that starts and ends at this node contributes both ends.
            const bool in = net.arrives_at(ref, node), out = net.leaves_from(ref, node);
            for (int end = 0; end < 2; ++end) {
                if (end == 0 ? !in : !out) continue;
                const Path stub = len <= reach ? path
                                  : end == 0   ? path.sub_path(len - reach, len, options.tolerance)
                                               : path.sub_path(0, reach, options.tolerance);
                const std::vector<Vec2> line = flatten(stub, options.tolerance);
                const float width = edge.tracks[t].width + extra;
                if (line.size() < 2 || width <= 0) continue;
                PolygonSet b = band(line, width, width, options.tolerance);
                if (!b.empty()) bands.push_back(std::move(b));
                if (len <= reach) break; // the whole track is in already
            }
        }
    }
    for (u32 c : net.connectors_at(node)) sweep_connector(net, net.connectors()[c], options, nullptr, bands);
    return finish(bands, options);
}

std::vector<Vec2> track_edge(const CurveNetwork& net, TrackRef track, float side, float inset, float tolerance) {
    if (net.track_path(track).empty()) return {};
    // The sides follow the taper of a lane that opens or closes, so a line
    // drawn along one follows it too.
    Sides sides = track_sides(net, track, tolerance, 0, inset);
    if (absf(side) < 1e-6f) return sides.centre;
    return side < 0 ? sides.left : sides.right;
}

std::vector<Vec2> connector_edge(const CurveNetwork& net, u32 connector, float side, float inset, float tolerance) {
    if (connector >= net.connectors().size()) return {};
    const Connector& k = net.connectors()[connector];
    std::vector<Vec2> line = flatten(k.path, tolerance);
    if (absf(side) < 1e-6f) return line;
    const float h0 = std::max(track_width(net, k.from) * 0.5f - inset, 0.0f);
    const float h1 = std::max(track_width(net, k.to) * 0.5f - inset, 0.0f);
    return offset_tapered(line, side * h0, side * h1);
}

} // namespace rw
