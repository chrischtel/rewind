#pragma once

// Areas: filled regions with holes, and the operations that turn a pile of
// overlapping shapes into one clean partition. Union, difference,
// intersection, growing and shrinking with round corners, turning a line into
// the band it sweeps, clipping lines to an area, and hatching.
//
// This is what lets a game stop stacking shapes on top of each other. Sweep
// every lane as a band, unite the bands, and the result is one outline with
// its holes; grow it for a kerb, subtract it from the ground, hatch what is
// left over. Every point of the plane then belongs to exactly one surface,
// so nothing overlaps and nothing shows through.
//
// Everything runs on an integer grid of 1/1024 of a unit inside, so the same
// input gives the same output on every machine: safe to call from tick().
// Pure math with no rendering dependency; build_polygon_set() in
// render/mesh.h turns a result into triangles.

#include "rewind/core/math.h"
#include "rewind/core/types.h"

#include <vector>

namespace rw {

/// A closed loop of points; the last point joins back to the first.
using Ring = std::vector<Vec2>;

/// One connected region: its boundary and the holes cut out of it. Results
/// from this module always have `outer` with positive polygon_area() and
/// every hole negative; inputs may be wound either way.
struct Polygon {
    Ring outer;
    std::vector<Ring> holes;
};

/// Any number of regions that do not overlap each other.
struct PolygonSet {
    std::vector<Polygon> polygons;

    bool empty() const { return polygons.empty(); }
    usize size() const { return polygons.size(); }
    void clear() { polygons.clear(); }
    /// Filled area: outers minus holes.
    float area() const;
    /// Inside a region and not inside one of its holes.
    bool contains(Vec2 p) const;
    Rect bounds() const;
    /// Every boundary, outers and holes alike, as closed loops.
    std::vector<Ring> rings() const;
    usize point_count() const;

    static PolygonSet from_ring(const Ring& ring);
    static PolygonSet from_rect(const Rect& r);
    static PolygonSet from_circle(Vec2 center, float radius, int segments = 48);
};

enum class PolyJoin : u8 {
    Round, ///< corners become arcs: kerbs, anything grown from a line
    Miter, ///< corners stay sharp up to the miter limit, then are cut off
    Bevel, ///< corners are cut straight across
};

enum class PolyCap : u8 {
    Butt,   ///< the band ends exactly at the line's end
    Square, ///< it ends half a width further
    Round,  ///< it ends in a half disc
};

struct OffsetOptions {
    PolyJoin join = PolyJoin::Round;
    float miter_limit = 2;        ///< in multiples of the distance
    float arc_tolerance = 0.02f;  ///< how far a round corner's chords may stray from the true arc
};

// ---- combining ----------------------------------------------------------------------------

/// Everything covered by any of the rings. They may overlap, touch, cross
/// themselves and be wound either way: this is the cleaner for raw shapes.
PolygonSet polygon_union(const std::vector<Ring>& rings);
PolygonSet polygon_union(const PolygonSet& a, const PolygonSet& b);
/// Several sets at once; much faster than uniting them pair by pair.
PolygonSet polygon_union(const std::vector<PolygonSet>& sets);
/// `a` with everything in `b` removed.
PolygonSet polygon_difference(const PolygonSet& a, const PolygonSet& b);
/// What `a` and `b` both cover.
PolygonSet polygon_intersection(const PolygonSet& a, const PolygonSet& b);
/// What exactly one of them covers.
PolygonSet polygon_xor(const PolygonSet& a, const PolygonSet& b);

// ---- growing and shrinking ----------------------------------------------------------------

/// Grow every region by `distance` (shrink when negative). Regions that grow
/// into each other merge; holes that shrink to nothing vanish, as do regions.
PolygonSet polygon_offset(const PolygonSet& set, float distance, const OffsetOptions& options = {});

/// Grow then shrink by `radius`: fills gaps and slivers narrower than twice
/// the radius and rounds every inward corner to it. The outline of a road
/// junction is the union of its lanes, closed by the kerb radius.
PolygonSet polygon_close(const PolygonSet& set, float radius, float arc_tolerance = 0.02f);

/// Shrink then grow by `radius`: removes spikes and necks narrower than twice
/// the radius and rounds every outward corner to it.
PolygonSet polygon_open(const PolygonSet& set, float radius, float arc_tolerance = 0.02f);

/// The ring of width `width` lying just outside the set: its kerb, its
/// outline, its verge. `gap` leaves room between the set and the band.
PolygonSet polygon_border(const PolygonSet& set, float width, float gap = 0, const OffsetOptions& options = {});

// ---- lines ----------------------------------------------------------------------------------

struct BandOptions {
    PolyJoin join = PolyJoin::Round;
    PolyCap cap = PolyCap::Butt;
    float miter_limit = 2;
    float arc_tolerance = 0.02f;
};

/// The area a line of the given width sweeps. A lane is the band of its
/// centre line; a closed line gives a ring-shaped band.
PolygonSet polyline_band(const std::vector<Vec2>& line, float width, const BandOptions& options = {},
                         bool closed = false);

/// The parts of `line` that lie inside the set (or outside it), in order
/// along the line. Markings are trimmed with this: a lane line is clipped out
/// of every area that has its own paint.
std::vector<std::vector<Vec2>> clip_polyline(const std::vector<Vec2>& line, const PolygonSet& set,
                                             bool keep_inside = true);

// ---- hatching -------------------------------------------------------------------------------

/// Parallel stripes of `stripe` width and `pitch` centre to centre, at
/// `angle` radians, cut to the set. Returned as area, so the stripes end
/// exactly on the boundary whatever its shape. `inset` keeps them that far
/// from the edge, which leaves room for a border line.
PolygonSet polygon_hatch(const PolygonSet& set, float angle, float pitch, float stripe, float inset = 0,
                         float phase = 0);

/// The same as centre lines instead of area, for drawing with a stroke.
std::vector<std::vector<Vec2>> polygon_hatch_lines(const PolygonSet& set, float angle, float pitch,
                                                   float inset = 0, float phase = 0);

// ---- tidying --------------------------------------------------------------------------------

/// Drop points that lie within `tolerance` of the line between their neighbours.
PolygonSet polygon_simplify(const PolygonSet& set, float tolerance);

/// Regions and holes smaller than `min_area` are dropped: holes are filled,
/// specks are removed.
PolygonSet polygon_remove_small(const PolygonSet& set, float min_area);

/// The holes of a set as regions of their own. What a junction's lanes leave
/// uncovered in the middle is exactly this, ready to become an island.
PolygonSet polygon_holes(const PolygonSet& set);

/// The set with every hole filled in.
PolygonSet polygon_fill_holes(const PolygonSet& set);

/// The largest circle that fits inside a region: its centre and radius, to
/// about `precision`. The place for a label, a sign, a tree on an island; and
/// the radius says how wide the region really is, which is what decides
/// whether a gap is an island or only worth hatching.
struct InnerCircle {
    Vec2 center;
    float radius = 0;
};
InnerCircle polygon_inner_circle(const Polygon& polygon, float precision = 0.1f);

/// Signed distance from `p` to the boundary of the set: negative inside.
float polygon_signed_distance(const PolygonSet& set, Vec2 p);

} // namespace rw
