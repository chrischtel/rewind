#pragma once

// Curves and paths: cubic Beziers, arcs, Catmull-Rom splines, arc-length
// sampling, closest point, split and join, flattening, and offsetting of
// polylines and polygons. Pure math with no rendering dependency; feed the
// flattened points to build_polyline() or a tilemap, or drive anything that
// follows a path. Everything here reflects, so paths live in saved state,
// scenes and the inspector.

#include "rewind/core/math.h"
#include "rewind/core/types.h"

#include <vector>

namespace rw {

/// Cubic Bezier segment. p0 and p3 are the end points, p1 and p2 the handles.
struct CubicBezier {
    Vec2 p0, p1, p2, p3;

    Vec2 point(float t) const;
    /// First derivative (not normalised). Zero at a degenerate end; use direction() for a unit vector.
    Vec2 derivative(float t) const;
    Vec2 direction(float t) const;
    /// Unit normal: direction rotated by 90 degrees (perp()).
    Vec2 normal(float t) const { return perp(direction(t)); }
    float curvature(float t) const;
    /// Arc length by Gauss-Legendre quadrature per subdivision; exact enough for any game use.
    float length(int subdivisions = 8) const;
    void split(float t, CubicBezier& first, CubicBezier& second) const;
    CubicBezier sub_curve(float t0, float t1) const;
    CubicBezier reversed() const { return {p3, p2, p1, p0}; }
    Rect bounds() const;

    /// Straight line as a Bezier (handles at thirds).
    static CubicBezier line(Vec2 a, Vec2 b);
    static CubicBezier from_quadratic(Vec2 p0, Vec2 control, Vec2 p2);
    /// Circular arc of at most 90 degrees; longer arcs use arc_to_beziers().
    static CubicBezier arc(Vec2 center, float radius, float start_angle, float sweep);

    void reflect(Visitor& v) {
        v.field("p0", p0);
        v.field("p1", p1);
        v.field("p2", p2);
        v.field("p3", p3);
    }
};

/// Circular arc, angles in radians, sweep may be negative.
struct Arc {
    Vec2 center;
    float radius = 1;
    float start = 0;
    float sweep = 3.14159265f;

    Vec2 point(float t) const;
    Vec2 direction(float t) const;
    float length() const { return absf(sweep) * radius; }
    Vec2 start_point() const { return point(0); }
    Vec2 end_point() const { return point(1); }

    void reflect(Visitor& v) {
        v.field("center", center);
        v.field("radius", radius, {.min = 0});
        v.field("start", start);
        v.field("sweep", sweep);
    }
};

/// Split an arc into Bezier segments of at most 90 degrees each. Appends.
void arc_to_beziers(const Arc& arc, std::vector<CubicBezier>& out);

/// A path is a chain of cubic segments joined end to end, open or closed.
/// Positions along it are addressed either by (segment, t) or by distance
/// from the start; PathSampler does the distance side quickly.
struct Path {
    std::vector<CubicBezier> segments;
    bool closed = false;

    bool empty() const { return segments.empty(); }
    Vec2 start() const { return segments.empty() ? Vec2{} : segments.front().p0; }
    Vec2 end() const { return segments.empty() ? Vec2{} : segments.back().p3; }
    float length() const;
    Rect bounds() const;
    Vec2 point(usize segment, float t) const { return segments[segment].point(t); }
    Vec2 direction(usize segment, float t) const { return segments[segment].direction(t); }

    // ---- building
    void move_to(Vec2 p);                 ///< Start a new path (clears).
    void line_to(Vec2 p);
    void quad_to(Vec2 control, Vec2 p);
    void cubic_to(Vec2 c1, Vec2 c2, Vec2 p);
    void arc_to(Vec2 center, float sweep); ///< Arc from the current point around `center`.
    void close();                           ///< Straight segment back to the start if needed, then closed = true.
    Vec2 current() const { return end(); }

    /// Straight segments through the points.
    static Path from_polyline(const std::vector<Vec2>& points, bool closed = false);
    /// Smooth curve through every point (Catmull-Rom converted to Beziers).
    /// `tension` 0.5 is the classic centripetal-looking curve; 0 is straighter, 1 rounder.
    static Path from_catmull_rom(const std::vector<Vec2>& points, bool closed = false, float tension = 0.5f);

    // ---- editing
    /// Cut at (segment, t) into two open paths.
    void split(usize segment, float t, Path& first, Path& second) const;
    /// Insert a point at (segment, t) without changing the shape (one segment becomes two).
    void subdivide(usize segment, float t);
    /// Append another path; a straight segment bridges any gap.
    void join(const Path& other);
    Path reversed() const;
    /// Transform every control point.
    void transform(const Mat4& m);

    // ---- geometry
    /// Polyline approximation. Adaptive: the result deviates from the curve
    /// by at most `tolerance`. Appends; the first point is included unless
    /// `skip_first`.
    void flatten(float tolerance, std::vector<Vec2>& out, bool skip_first = false) const;

    /// Parallel curve at `distance` (positive = perp() side, i.e. right of
    /// travel in y-down space), flattened and offset as a polyline. Sharp
    /// corners are mitered up to `miter_limit`, then bevelled.
    std::vector<Vec2> offset(float distance, float tolerance = 0.25f, float miter_limit = 4) const;

    void reflect(Visitor& v) {
        v.hint(Widget::Path);
        v.field("segments", segments);
        v.field("closed", closed);
    }
};

/// Position and heading somewhere on a path.
struct PathPoint {
    Vec2 position;
    Vec2 direction; ///< unit tangent
    float distance = 0; ///< from the start of the path
    usize segment = 0;
    float t = 0;
};

/// Arc-length lookup for a Path: distance to point, point to distance,
/// closest point. Built once, reused while the path does not change.
class PathSampler {
public:
    PathSampler() = default;
    explicit PathSampler(const Path& path, float tolerance = 0.25f) { build(path, tolerance); }

    void build(const Path& path, float tolerance = 0.25f);
    bool empty() const { return samples_.empty(); }
    float length() const { return samples_.empty() ? 0 : samples_.back().distance; }
    bool closed() const { return closed_; }

    /// Point at `distance` from the start. Clamped on open paths, wrapped on closed ones.
    PathPoint at(float distance) const;
    /// Closest point on the path to `p` (refined on the exact curve).
    PathPoint closest(Vec2 p) const;
    float distance_to(Vec2 p) const { return distance(closest(p).position, p); }

    /// The flattened points, in order (the last one equals the first on closed paths).
    std::vector<Vec2> points() const;

private:
    struct Sample {
        Vec2 position;
        float distance;
        usize segment;
        float t;
    };
    std::vector<Sample> samples_;
    std::vector<CubicBezier> segments_;
    bool closed_ = false;
};

// ---- polylines and polygons ------------------------------------------------------------

/// Parallel polyline at `distance` (positive = perp() side). For a closed
/// polygon this grows (or shrinks, negative) it. Corners are mitered up to
/// `miter_limit` times the distance and bevelled beyond. Self-intersections
/// from shrinking below a feature's size are not removed.
std::vector<Vec2> offset_polyline(const std::vector<Vec2>& points, float distance, bool closed, float miter_limit = 4);

/// Drop points that are within `tolerance` of the line between their neighbours (Douglas-Peucker).
std::vector<Vec2> simplify_polyline(const std::vector<Vec2>& points, float tolerance);

/// Resample a polyline at a fixed spacing. Keeps the first point; the last
/// point is added when it is more than half a step from the previous sample.
std::vector<Vec2> resample_polyline(const std::vector<Vec2>& points, float spacing, bool closed = false);

/// Closest point on segment ab to p, and the parameter along ab in [0, 1].
Vec2 closest_point_on_segment(Vec2 a, Vec2 b, Vec2 p, float* t = nullptr);

/// Intersection of two infinite lines through (a, a + ad) and (b, b + bd). False when parallel.
bool line_intersection(Vec2 a, Vec2 ad, Vec2 b, Vec2 bd, Vec2& out);

/// Whether segments ab and cd cross (touching counts).
bool segments_intersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Vec2* where = nullptr);

/// Point-in-polygon by winding (works for concave, any orientation).
bool polygon_contains(const std::vector<Vec2>& polygon, Vec2 p);

/// Convex hull (Andrew's monotone chain), counter-clockwise in y-up space.
std::vector<Vec2> convex_hull(std::vector<Vec2> points);

} // namespace rw
