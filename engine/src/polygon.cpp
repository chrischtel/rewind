#include "rewind/core/polygon.h"

#include "rewind/core/curve.h"

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <queue>

namespace rw {

namespace {

using Clipper2Lib::Path64;
using Clipper2Lib::Paths64;
using Clipper2Lib::Point64;
using Clipper2Lib::PolyPath64;
using Clipper2Lib::PolyTree64;

// One unit is 1024 grid steps: about a millimetre when a unit is a metre, and
// a power of two so nothing is lost going in and out of the grid.
constexpr double kScale = 1024.0;

Point64 to_grid(Vec2 p) {
    return Point64(static_cast<int64_t>(std::llround(static_cast<double>(p.x) * kScale)),
                   static_cast<int64_t>(std::llround(static_cast<double>(p.y) * kScale)));
}

Vec2 from_grid(const Point64& p) {
    return Vec2{static_cast<float>(static_cast<double>(p.x) / kScale),
                static_cast<float>(static_cast<double>(p.y) / kScale)};
}

Path64 to_path(const std::vector<Vec2>& points) {
    Path64 path;
    path.reserve(points.size());
    for (const Vec2& p : points) {
        const Point64 q = to_grid(p);
        if (!path.empty() && path.back() == q) continue;
        path.push_back(q);
    }
    return path;
}

Ring from_path(const Path64& path) {
    Ring ring;
    ring.reserve(path.size());
    for (const Point64& p : path) ring.push_back(from_grid(p));
    return ring;
}

/// A closed path wound the way asked: positive area for boundaries, negative for holes.
Path64 to_ring(const Ring& ring, bool positive) {
    Path64 path = to_path(ring);
    if (path.size() > 1 && path.front() == path.back()) path.pop_back();
    if (path.size() < 3) return {};
    if ((Clipper2Lib::Area(path) > 0) != positive) std::reverse(path.begin(), path.end());
    return path;
}

Paths64 to_paths(const PolygonSet& set) {
    Paths64 paths;
    for (const Polygon& poly : set.polygons) {
        Path64 outer = to_ring(poly.outer, true);
        if (outer.empty()) continue;
        paths.push_back(std::move(outer));
        for (const Ring& hole : poly.holes) {
            Path64 h = to_ring(hole, false);
            if (!h.empty()) paths.push_back(std::move(h));
        }
    }
    return paths;
}

/// A boundary and its holes; whatever sits inside a hole is a region of its own.
void collect(const PolyPath64& outer, PolygonSet& out) {
    Polygon poly;
    poly.outer = from_path(outer.Polygon());
    for (usize i = 0; i < outer.Count(); ++i) {
        const PolyPath64& hole = *outer[i];
        poly.holes.push_back(from_path(hole.Polygon()));
        for (usize k = 0; k < hole.Count(); ++k) collect(*hole[k], out);
    }
    if (poly.outer.size() >= 3) out.polygons.push_back(std::move(poly));
}

PolygonSet from_tree(const PolyTree64& tree) {
    PolygonSet out;
    for (usize i = 0; i < tree.Count(); ++i) collect(*tree[i], out);
    return out;
}

PolygonSet boolean(Clipper2Lib::ClipType type, const Paths64& subject, const Paths64& clip) {
    Clipper2Lib::Clipper64 c;
    c.AddSubject(subject);
    if (!clip.empty()) c.AddClip(clip);
    PolyTree64 tree;
    c.Execute(type, Clipper2Lib::FillRule::NonZero, tree);
    return from_tree(tree);
}

Clipper2Lib::JoinType join_of(PolyJoin join) {
    switch (join) {
    case PolyJoin::Miter: return Clipper2Lib::JoinType::Miter;
    case PolyJoin::Bevel: return Clipper2Lib::JoinType::Bevel;
    default: return Clipper2Lib::JoinType::Round;
    }
}

PolygonSet inflate(const Paths64& paths, float distance, PolyJoin join, Clipper2Lib::EndType end, float miter_limit,
                   float arc_tolerance) {
    if (paths.empty()) return {};
    Clipper2Lib::ClipperOffset offset(std::max(static_cast<double>(miter_limit), 1.0),
                                      std::max(static_cast<double>(arc_tolerance), 0.0005) * kScale);
    offset.AddPaths(paths, join_of(join), end);
    PolyTree64 tree;
    offset.Execute(static_cast<double>(distance) * kScale, tree);
    return from_tree(tree);
}

float ring_area(const Ring& ring) {
    double sum = 0;
    for (usize i = 0; i < ring.size(); ++i) {
        const Vec2 a = ring[i], b = ring[(i + 1) % ring.size()];
        sum += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
    }
    return static_cast<float>(sum * 0.5);
}

float distance_to_ring(const Ring& ring, Vec2 p) {
    float best = 1e30f;
    for (usize i = 0; i < ring.size(); ++i) {
        const Vec2 q = closest_point_on_segment(ring[i], ring[(i + 1) % ring.size()], p);
        best = std::min(best, length_sq(q - p));
    }
    return std::sqrt(best);
}

/// Signed distance into one region: positive inside, negative outside or in a hole.
float depth_in(const Polygon& poly, Vec2 p) {
    float d = distance_to_ring(poly.outer, p);
    bool inside = polygon_contains(poly.outer, p);
    for (const Ring& hole : poly.holes) {
        d = std::min(d, distance_to_ring(hole, p));
        if (inside && polygon_contains(hole, p)) inside = false;
    }
    return inside ? d : -d;
}

/// How far along `line` the point nearest to `p` lies.
float along_polyline(const std::vector<Vec2>& line, Vec2 p) {
    float best = 1e30f, best_at = 0, walked = 0;
    for (usize i = 0; i + 1 < line.size(); ++i) {
        float t = 0;
        const Vec2 q = closest_point_on_segment(line[i], line[i + 1], p, &t);
        const float d = length_sq(q - p);
        const float len = distance(line[i], line[i + 1]);
        if (d < best) {
            best = d;
            best_at = walked + len * t;
        }
        walked += len;
    }
    return best_at;
}

Rect bounds_of(const std::vector<Ring>& rings) {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (const Ring& ring : rings)
        for (const Vec2& p : ring) {
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
    if (x1 < x0) return {};
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

/// The stripes of a hatch across `area`: for each, where its centre line
/// starts and ends, long enough to cross the whole area at that angle.
struct Stripe {
    Vec2 from, to;
};
std::vector<Stripe> stripes_across(const Rect& area, float angle, float pitch, float phase) {
    std::vector<Stripe> out;
    if (pitch <= 1e-4f || area.w <= 0 || area.h <= 0) return out;
    const Vec2 along = from_angle(angle);
    const Vec2 across = perp(along);
    const Vec2 corners[4] = {{area.x, area.y}, {area.x + area.w, area.y}, {area.x + area.w, area.y + area.h},
                             {area.x, area.y + area.h}};
    float a0 = 1e30f, a1 = -1e30f, c0 = 1e30f, c1 = -1e30f;
    for (const Vec2& c : corners) {
        a0 = std::min(a0, dot(c, along));
        a1 = std::max(a1, dot(c, along));
        c0 = std::min(c0, dot(c, across));
        c1 = std::max(c1, dot(c, across));
    }
    const int first = static_cast<int>(std::floor((c0 - phase) / pitch)) - 1;
    const int last = static_cast<int>(std::ceil((c1 - phase) / pitch)) + 1;
    if (last - first > 20000) return out; // a pitch far too fine for this area
    for (int k = first; k <= last; ++k) {
        const float c = phase + static_cast<float>(k) * pitch;
        out.push_back(Stripe{along * (a0 - 1.0f) + across * c, along * (a1 + 1.0f) + across * c});
    }
    return out;
}

} // namespace

// ---- PolygonSet ---------------------------------------------------------------------------

float PolygonSet::area() const {
    float total = 0;
    for (const Polygon& poly : polygons) {
        total += absf(ring_area(poly.outer));
        for (const Ring& hole : poly.holes) total -= absf(ring_area(hole));
    }
    return total;
}

bool PolygonSet::contains(Vec2 p) const {
    for (const Polygon& poly : polygons) {
        if (!polygon_contains(poly.outer, p)) continue;
        bool in_hole = false;
        for (const Ring& hole : poly.holes)
            if (polygon_contains(hole, p)) {
                in_hole = true;
                break;
            }
        if (!in_hole) return true;
    }
    return false;
}

Rect PolygonSet::bounds() const {
    std::vector<Ring> outers;
    outers.reserve(polygons.size());
    for (const Polygon& poly : polygons) outers.push_back(poly.outer);
    return bounds_of(outers);
}

std::vector<Ring> PolygonSet::rings() const {
    std::vector<Ring> out;
    for (const Polygon& poly : polygons) {
        out.push_back(poly.outer);
        for (const Ring& hole : poly.holes) out.push_back(hole);
    }
    return out;
}

usize PolygonSet::point_count() const {
    usize n = 0;
    for (const Polygon& poly : polygons) {
        n += poly.outer.size();
        for (const Ring& hole : poly.holes) n += hole.size();
    }
    return n;
}

PolygonSet PolygonSet::from_ring(const Ring& ring) { return polygon_union(std::vector<Ring>{ring}); }

PolygonSet PolygonSet::from_rect(const Rect& r) {
    return from_ring(Ring{{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}});
}

PolygonSet PolygonSet::from_circle(Vec2 center, float radius, int segments) {
    Ring ring;
    segments = std::max(segments, 3);
    ring.reserve(static_cast<usize>(segments));
    for (int i = 0; i < segments; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(segments);
        ring.push_back(center + from_angle(a) * radius);
    }
    return from_ring(ring);
}

// ---- combining ----------------------------------------------------------------------------

PolygonSet polygon_union(const std::vector<Ring>& rings) {
    Paths64 paths;
    paths.reserve(rings.size());
    for (const Ring& ring : rings) {
        Path64 p = to_ring(ring, true);
        if (!p.empty()) paths.push_back(std::move(p));
    }
    if (paths.empty()) return {};
    return boolean(Clipper2Lib::ClipType::Union, paths, {});
}

PolygonSet polygon_union(const PolygonSet& a, const PolygonSet& b) {
    return boolean(Clipper2Lib::ClipType::Union, to_paths(a), to_paths(b));
}

PolygonSet polygon_union(const std::vector<PolygonSet>& sets) {
    // Throwing every boundary and hole into one union would let a hole of one
    // set punch through another set lying over it: a hole winds the other way
    // and cancels whatever covers it. So the sets are united two at a time,
    // as a balanced tree, which keeps the whole thing near n log n.
    if (sets.empty()) return {};
    std::vector<Paths64> level;
    level.reserve(sets.size());
    for (const PolygonSet& s : sets) level.push_back(to_paths(s));
    while (level.size() > 1) {
        std::vector<Paths64> next;
        next.reserve((level.size() + 1) / 2);
        for (usize i = 0; i + 1 < level.size(); i += 2) {
            Clipper2Lib::Clipper64 c;
            c.AddSubject(level[i]);
            c.AddClip(level[i + 1]);
            Paths64 merged;
            c.Execute(Clipper2Lib::ClipType::Union, Clipper2Lib::FillRule::NonZero, merged);
            next.push_back(std::move(merged));
        }
        if (level.size() % 2 == 1) next.push_back(std::move(level.back()));
        level = std::move(next);
    }
    return boolean(Clipper2Lib::ClipType::Union, level.front(), {});
}

PolygonSet polygon_difference(const PolygonSet& a, const PolygonSet& b) {
    return boolean(Clipper2Lib::ClipType::Difference, to_paths(a), to_paths(b));
}

PolygonSet polygon_intersection(const PolygonSet& a, const PolygonSet& b) {
    return boolean(Clipper2Lib::ClipType::Intersection, to_paths(a), to_paths(b));
}

PolygonSet polygon_xor(const PolygonSet& a, const PolygonSet& b) {
    return boolean(Clipper2Lib::ClipType::Xor, to_paths(a), to_paths(b));
}

// ---- growing and shrinking ----------------------------------------------------------------

PolygonSet polygon_offset(const PolygonSet& set, float distance, const OffsetOptions& options) {
    if (absf(distance) < 1e-6f) return set;
    return inflate(to_paths(set), distance, options.join, Clipper2Lib::EndType::Polygon, options.miter_limit,
                   options.arc_tolerance);
}

PolygonSet polygon_close(const PolygonSet& set, float radius, float arc_tolerance) {
    if (radius <= 0) return set;
    OffsetOptions o;
    o.arc_tolerance = arc_tolerance;
    return polygon_offset(polygon_offset(set, radius, o), -radius, o);
}

PolygonSet polygon_open(const PolygonSet& set, float radius, float arc_tolerance) {
    if (radius <= 0) return set;
    OffsetOptions o;
    o.arc_tolerance = arc_tolerance;
    return polygon_offset(polygon_offset(set, -radius, o), radius, o);
}

PolygonSet polygon_border(const PolygonSet& set, float width, float gap, const OffsetOptions& options) {
    if (width <= 0) return {};
    const PolygonSet inner = gap > 0 ? polygon_offset(set, gap, options) : set;
    return polygon_difference(polygon_offset(set, gap + width, options), inner);
}

// ---- lines ----------------------------------------------------------------------------------

PolygonSet polyline_band(const std::vector<Vec2>& line, float width, const BandOptions& options, bool closed) {
    if (line.size() < 2 || width <= 0) return {};
    Path64 path = to_path(line);
    if (closed && path.size() > 1 && path.front() == path.back()) path.pop_back();
    if (path.size() < 2) return {};
    Clipper2Lib::EndType end = Clipper2Lib::EndType::Butt;
    if (closed) end = Clipper2Lib::EndType::Joined;
    else if (options.cap == PolyCap::Square) end = Clipper2Lib::EndType::Square;
    else if (options.cap == PolyCap::Round) end = Clipper2Lib::EndType::Round;
    return inflate(Paths64{path}, width * 0.5f, options.join, end, options.miter_limit, options.arc_tolerance);
}

std::vector<std::vector<Vec2>> clip_polyline(const std::vector<Vec2>& line, const PolygonSet& set, bool keep_inside) {
    std::vector<std::vector<Vec2>> out;
    if (line.size() < 2) return out;
    if (set.empty()) {
        if (!keep_inside) out.push_back(line);
        return out;
    }
    Clipper2Lib::Clipper64 c;
    c.AddOpenSubject(Paths64{to_path(line)});
    c.AddClip(to_paths(set));
    Paths64 closed_paths, open_paths;
    c.Execute(keep_inside ? Clipper2Lib::ClipType::Intersection : Clipper2Lib::ClipType::Difference,
              Clipper2Lib::FillRule::NonZero, closed_paths, open_paths);

    // The clipper hands the pieces back in no particular order or direction.
    struct Piece {
        float at;
        std::vector<Vec2> points;
    };
    std::vector<Piece> pieces;
    for (const Path64& p : open_paths) {
        if (p.size() < 2) continue;
        std::vector<Vec2> points = from_path(p);
        float head = along_polyline(line, points.front());
        const float tail = along_polyline(line, points.back());
        if (tail < head) {
            std::reverse(points.begin(), points.end());
            head = tail;
        }
        pieces.push_back(Piece{head, std::move(points)});
    }
    std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.at < b.at; });
    out.reserve(pieces.size());
    for (Piece& p : pieces) out.push_back(std::move(p.points));
    return out;
}

// ---- hatching -------------------------------------------------------------------------------

PolygonSet polygon_hatch(const PolygonSet& set, float angle, float pitch, float stripe, float inset, float phase) {
    if (set.empty() || stripe <= 0 || pitch <= 0) return {};
    const PolygonSet area = inset > 0 ? polygon_offset(set, -inset) : set;
    if (area.empty()) return {};
    const Vec2 across = perp(from_angle(angle));
    Paths64 bars;
    for (const Stripe& s : stripes_across(area.bounds(), angle, pitch, phase)) {
        const Vec2 half = across * (stripe * 0.5f);
        Path64 bar = to_ring(Ring{s.from - half, s.to - half, s.to + half, s.from + half}, true);
        if (!bar.empty()) bars.push_back(std::move(bar));
    }
    if (bars.empty()) return {};
    return boolean(Clipper2Lib::ClipType::Intersection, bars, to_paths(area));
}

std::vector<std::vector<Vec2>> polygon_hatch_lines(const PolygonSet& set, float angle, float pitch, float inset,
                                                   float phase) {
    std::vector<std::vector<Vec2>> out;
    if (set.empty() || pitch <= 0) return out;
    const PolygonSet area = inset > 0 ? polygon_offset(set, -inset) : set;
    if (area.empty()) return out;
    for (const Stripe& s : stripes_across(area.bounds(), angle, pitch, phase))
        for (std::vector<Vec2>& piece : clip_polyline({s.from, s.to}, area, true)) out.push_back(std::move(piece));
    return out;
}

// ---- tidying --------------------------------------------------------------------------------

PolygonSet polygon_simplify(const PolygonSet& set, float tolerance) {
    if (tolerance <= 0 || set.empty()) return set;
    const Paths64 simple = Clipper2Lib::SimplifyPaths(to_paths(set), static_cast<double>(tolerance) * kScale, true);
    // Dropping points can make a boundary touch itself; one union puts it right.
    return boolean(Clipper2Lib::ClipType::Union, simple, {});
}

PolygonSet polygon_remove_small(const PolygonSet& set, float min_area) {
    PolygonSet out;
    for (const Polygon& poly : set.polygons) {
        if (absf(ring_area(poly.outer)) < min_area) continue;
        Polygon kept;
        kept.outer = poly.outer;
        for (const Ring& hole : poly.holes)
            if (absf(ring_area(hole)) >= min_area) kept.holes.push_back(hole);
        out.polygons.push_back(std::move(kept));
    }
    return out;
}

PolygonSet polygon_fill_holes(const PolygonSet& set) {
    std::vector<Ring> outers;
    outers.reserve(set.polygons.size());
    for (const Polygon& poly : set.polygons) outers.push_back(poly.outer);
    return polygon_union(outers);
}

PolygonSet polygon_holes(const PolygonSet& set) { return polygon_difference(polygon_fill_holes(set), set); }

InnerCircle polygon_inner_circle(const Polygon& polygon, float precision) {
    // The pole of inaccessibility: search squares, best first, discarding any
    // that could not beat the best centre found so far even at their corner.
    InnerCircle best;
    if (polygon.outer.size() < 3) return best;
    const Rect box = bounds_of({polygon.outer});
    const float cell = std::min(box.w, box.h);
    if (cell <= 0) return best;
    precision = std::max(precision, cell * 1e-4f);

    struct Cell {
        Vec2 c;
        float half;
        float d;   ///< depth of the centre
        float max; ///< the best any point of the cell could do
    };
    const auto make = [&](Vec2 c, float half) {
        const float d = depth_in(polygon, c);
        return Cell{c, half, d, d + half * 1.41421356f};
    };
    const auto worse = [](const Cell& a, const Cell& b) { return a.max < b.max; };
    std::priority_queue<Cell, std::vector<Cell>, decltype(worse)> queue(worse);

    float half = cell * 0.5f;
    for (float x = box.x; x < box.x + box.w; x += cell)
        for (float y = box.y; y < box.y + box.h; y += cell) queue.push(make(Vec2{x + half, y + half}, half));

    // A first guess: the middle of the box, and the centroid of the boundary.
    Cell champion = make(Vec2{box.x + box.w * 0.5f, box.y + box.h * 0.5f}, 0);
    Vec2 centroid{};
    for (const Vec2& p : polygon.outer) centroid = centroid + p;
    const Cell mid = make(centroid * (1.0f / static_cast<float>(polygon.outer.size())), 0);
    if (mid.d > champion.d) champion = mid;

    int guard = 0;
    while (!queue.empty() && guard++ < 20000) {
        const Cell top = queue.top();
        queue.pop();
        if (top.d > champion.d) champion = top;
        if (top.max - champion.d <= precision) continue;
        half = top.half * 0.5f;
        queue.push(make(Vec2{top.c.x - half, top.c.y - half}, half));
        queue.push(make(Vec2{top.c.x + half, top.c.y - half}, half));
        queue.push(make(Vec2{top.c.x - half, top.c.y + half}, half));
        queue.push(make(Vec2{top.c.x + half, top.c.y + half}, half));
    }
    best.center = champion.c;
    best.radius = std::max(champion.d, 0.0f);
    return best;
}

float polygon_signed_distance(const PolygonSet& set, Vec2 p) {
    float best = 1e30f;
    for (const Polygon& poly : set.polygons) {
        best = std::min(best, distance_to_ring(poly.outer, p));
        for (const Ring& hole : poly.holes) best = std::min(best, distance_to_ring(hole, p));
    }
    return set.contains(p) ? -best : best;
}

} // namespace rw
