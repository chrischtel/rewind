#include "rewind/core/curve.h"

#include <algorithm>
#include <cmath>

namespace rw {

namespace {

constexpr float kPi = 3.14159265f;
/// Points closer than this are the same point (seams of closed paths, duplicates).
constexpr float kWeld = 1e-4f;

/// Second derivative of a cubic Bezier.
Vec2 second_derivative(const CubicBezier& b, float t) {
    const float mt = 1 - t;
    return 6 * mt * (b.p2 - 2 * b.p1 + b.p0) + 6 * t * (b.p3 - 2 * b.p2 + b.p1);
}

/// Adaptive subdivision: emits (t, point) pairs for t in (t0, t1], never the
/// start point, so segments chain without duplicates.
template <class Emit>
void flatten_recursive(const CubicBezier& b, float t0, float t1, float tolerance, int depth, Emit&& emit) {
    // Flat enough when both handles lie within tolerance of the chord.
    const Vec2 chord = b.p3 - b.p0;
    const float len = length(chord);
    float d1, d2;
    if (len < 1e-6f) {
        d1 = distance(b.p1, b.p0);
        d2 = distance(b.p2, b.p0);
    } else {
        d1 = absf(cross(b.p1 - b.p0, chord)) / len;
        d2 = absf(cross(b.p2 - b.p0, chord)) / len;
    }
    if ((d1 <= tolerance && d2 <= tolerance) || depth >= 16) {
        emit(t1, b.p3);
        return;
    }
    CubicBezier a, c;
    b.split(0.5f, a, c);
    const float tm = (t0 + t1) * 0.5f;
    flatten_recursive(a, t0, tm, tolerance, depth + 1, emit);
    flatten_recursive(c, tm, t1, tolerance, depth + 1, emit);
}

void quad_roots(float a, float b, float c, float* roots, int& count) {
    count = 0;
    if (absf(a) < 1e-8f) {
        if (absf(b) > 1e-8f) roots[count++] = -c / b;
        return;
    }
    const float disc = b * b - 4 * a * c;
    if (disc < 0) return;
    const float s = std::sqrt(disc);
    roots[count++] = (-b + s) / (2 * a);
    roots[count++] = (-b - s) / (2 * a);
}

} // namespace

// ---- CubicBezier -----------------------------------------------------------------------

Vec2 CubicBezier::point(float t) const {
    const float mt = 1 - t;
    const float a = mt * mt * mt, b = 3 * mt * mt * t, c = 3 * mt * t * t, d = t * t * t;
    return a * p0 + b * p1 + c * p2 + d * p3;
}

Vec2 CubicBezier::derivative(float t) const {
    const float mt = 1 - t;
    return 3 * mt * mt * (p1 - p0) + 6 * mt * t * (p2 - p1) + 3 * t * t * (p3 - p2);
}

Vec2 CubicBezier::direction(float t) const {
    Vec2 d = derivative(t);
    if (length_sq(d) < 1e-10f) {
        // Degenerate end (handle on the end point): nudge inwards.
        d = derivative(t < 0.5f ? t + 1e-3f : t - 1e-3f);
        if (length_sq(d) < 1e-10f) d = p3 - p0;
    }
    return normalize(d);
}

float CubicBezier::curvature(float t) const {
    const Vec2 d1 = derivative(t);
    const Vec2 d2 = second_derivative(*this, t);
    const float l = std::sqrt(length_sq(d1));
    if (l < 1e-6f) return 0;
    return cross(d1, d2) / (l * l * l);
}

float CubicBezier::length(int subdivisions) const {
    // 5-point Gauss-Legendre per subdivision.
    static const float nodes[5] = {0.0f, -0.5384693f, 0.5384693f, -0.9061798f, 0.9061798f};
    static const float weights[5] = {0.5688889f, 0.4786287f, 0.4786287f, 0.2369269f, 0.2369269f};
    if (subdivisions < 1) subdivisions = 1;
    float total = 0;
    const float h = 1.0f / static_cast<float>(subdivisions);
    for (int s = 0; s < subdivisions; ++s) {
        const float a = static_cast<float>(s) * h, mid = a + h * 0.5f;
        float sum = 0;
        for (int i = 0; i < 5; ++i) sum += weights[i] * std::sqrt(length_sq(derivative(mid + nodes[i] * h * 0.5f)));
        total += sum * h * 0.5f;
    }
    return total;
}

void CubicBezier::split(float t, CubicBezier& first, CubicBezier& second) const {
    const Vec2 p01 = lerp(p0, p1, t), p12 = lerp(p1, p2, t), p23 = lerp(p2, p3, t);
    const Vec2 p012 = lerp(p01, p12, t), p123 = lerp(p12, p23, t);
    const Vec2 mid = lerp(p012, p123, t);
    first = {p0, p01, p012, mid};
    second = {mid, p123, p23, p3};
}

CubicBezier CubicBezier::sub_curve(float t0, float t1) const {
    if (t0 > t1) std::swap(t0, t1);
    CubicBezier a, b, c, d;
    split(t1, a, b);
    if (t1 <= 1e-6f) return a;
    a.split(t0 / t1, c, d);
    return d;
}

Rect CubicBezier::bounds() const {
    float min_x = std::min(p0.x, p3.x), max_x = std::max(p0.x, p3.x);
    float min_y = std::min(p0.y, p3.y), max_y = std::max(p0.y, p3.y);
    // Derivative is a quadratic: 3[(-p0 + 3p1 - 3p2 + p3) t^2 + 2(p0 - 2p1 + p2) t + (p1 - p0)].
    const Vec2 a = -1.0f * p0 + 3.0f * p1 - 3.0f * p2 + p3;
    const Vec2 b = 2.0f * (p0 - 2.0f * p1 + p2);
    const Vec2 c = p1 - p0;
    float roots[2];
    int count;
    quad_roots(a.x, b.x, c.x, roots, count);
    for (int i = 0; i < count; ++i)
        if (roots[i] > 0 && roots[i] < 1) {
            const float x = point(roots[i]).x;
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
        }
    quad_roots(a.y, b.y, c.y, roots, count);
    for (int i = 0; i < count; ++i)
        if (roots[i] > 0 && roots[i] < 1) {
            const float y = point(roots[i]).y;
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
    return {min_x, min_y, max_x - min_x, max_y - min_y};
}

CubicBezier CubicBezier::line(Vec2 a, Vec2 b) { return {a, lerp(a, b, 1.0f / 3), lerp(a, b, 2.0f / 3), b}; }

CubicBezier CubicBezier::from_quadratic(Vec2 q0, Vec2 control, Vec2 q2) {
    return {q0, q0 + (control - q0) * (2.0f / 3), q2 + (control - q2) * (2.0f / 3), q2};
}

CubicBezier CubicBezier::arc(Vec2 center, float radius, float start_angle, float sweep) {
    const float k = 4.0f / 3 * std::tan(sweep / 4);
    const Vec2 s = from_angle(start_angle), e = from_angle(start_angle + sweep);
    const Vec2 a = center + s * radius, b = center + e * radius;
    return {a, a + perp(s) * (k * radius), b - perp(e) * (k * radius), b};
}

// ---- Arc -------------------------------------------------------------------------------

Vec2 Arc::point(float t) const { return center + from_angle(start + sweep * t) * radius; }

Vec2 Arc::direction(float t) const {
    const Vec2 d = perp(from_angle(start + sweep * t));
    return sweep >= 0 ? d : -d;
}

void arc_to_beziers(const Arc& arc, std::vector<CubicBezier>& out) {
    const int pieces = std::max(1, static_cast<int>(std::ceil(absf(arc.sweep) / (kPi / 2) - 1e-4f)));
    const float step = arc.sweep / static_cast<float>(pieces);
    for (int i = 0; i < pieces; ++i)
        out.push_back(CubicBezier::arc(arc.center, arc.radius, arc.start + step * static_cast<float>(i), step));
}

// ---- Path ------------------------------------------------------------------------------

float Path::length() const {
    float total = 0;
    for (const CubicBezier& s : segments) total += s.length();
    return total;
}

Rect Path::bounds() const {
    if (segments.empty()) return {};
    Rect r = segments[0].bounds();
    for (usize i = 1; i < segments.size(); ++i) {
        const Rect b = segments[i].bounds();
        const float x0 = std::min(r.x, b.x), y0 = std::min(r.y, b.y);
        const float x1 = std::max(r.right(), b.right()), y1 = std::max(r.bottom(), b.bottom());
        r = {x0, y0, x1 - x0, y1 - y0};
    }
    return r;
}

void Path::move_to(Vec2 p) {
    segments.clear();
    closed = false;
    // A zero-length segment carries the start point until something real is added.
    segments.push_back({p, p, p, p});
}

namespace {
/// Drop the placeholder segment move_to() leaves behind.
Vec2 take_start(Path& path) {
    if (path.segments.size() == 1) {
        const CubicBezier& s = path.segments[0];
        if (s.p0 == s.p3 && s.p1 == s.p0 && s.p2 == s.p0) {
            const Vec2 p = s.p0;
            path.segments.clear();
            return p;
        }
    }
    return path.end();
}
} // namespace

void Path::line_to(Vec2 p) {
    const Vec2 from = take_start(*this);
    segments.push_back(CubicBezier::line(from, p));
}

void Path::quad_to(Vec2 control, Vec2 p) {
    const Vec2 from = take_start(*this);
    segments.push_back(CubicBezier::from_quadratic(from, control, p));
}

void Path::cubic_to(Vec2 c1, Vec2 c2, Vec2 p) {
    const Vec2 from = take_start(*this);
    segments.push_back({from, c1, c2, p});
}

void Path::arc_to(Vec2 center, float sweep) {
    const Vec2 from = take_start(*this);
    Arc arc;
    arc.center = center;
    arc.radius = distance(from, center);
    arc.start = angle_of(from - center);
    arc.sweep = sweep;
    arc_to_beziers(arc, segments);
}

void Path::close() {
    if (segments.empty()) return;
    if (distance(start(), end()) > kWeld) segments.push_back(CubicBezier::line(end(), start()));
    else segments.back().p3 = start(); // arcs end a rounding error away: snap
    closed = true;
}

Path Path::from_polyline(const std::vector<Vec2>& points, bool closed) {
    Path p;
    for (usize i = 0; i + 1 < points.size(); ++i) p.segments.push_back(CubicBezier::line(points[i], points[i + 1]));
    if (closed && points.size() > 2) {
        p.segments.push_back(CubicBezier::line(points.back(), points.front()));
        p.closed = true;
    }
    return p;
}

Path Path::from_catmull_rom(const std::vector<Vec2>& points, bool closed, float tension) {
    Path p;
    const usize n = points.size();
    if (n < 2) return p;
    const usize seg_count = closed ? n : n - 1;
    auto at = [&](std::ptrdiff_t i) -> Vec2 {
        if (closed) return points[static_cast<usize>((i + static_cast<std::ptrdiff_t>(n)) % static_cast<std::ptrdiff_t>(n))];
        return points[static_cast<usize>(std::clamp<std::ptrdiff_t>(i, 0, static_cast<std::ptrdiff_t>(n) - 1))];
    };
    for (usize i = 0; i < seg_count; ++i) {
        const auto k = static_cast<std::ptrdiff_t>(i);
        const Vec2 a = at(k - 1), b = at(k), c = at(k + 1), d = at(k + 2);
        p.segments.push_back({b, b + (c - a) * (tension / 3), c - (d - b) * (tension / 3), c});
    }
    p.closed = closed;
    return p;
}

void Path::split(usize segment, float t, Path& first, Path& second) const {
    first = Path{};
    second = Path{};
    if (segment >= segments.size()) {
        first = *this;
        first.closed = false;
        return;
    }
    CubicBezier a, b;
    segments[segment].split(t, a, b);
    if (closed) {
        // One cut on a loop gives one open path starting at the cut.
        first.segments.push_back(b);
        for (usize i = 1; i < segments.size(); ++i) first.segments.push_back(segments[(segment + i) % segments.size()]);
        first.segments.push_back(a);
        return;
    }
    first.segments.assign(segments.begin(), segments.begin() + static_cast<std::ptrdiff_t>(segment));
    first.segments.push_back(a);
    second.segments.push_back(b);
    second.segments.insert(second.segments.end(), segments.begin() + static_cast<std::ptrdiff_t>(segment) + 1,
                           segments.end());
}

void Path::subdivide(usize segment, float t) {
    if (segment >= segments.size()) return;
    CubicBezier a, b;
    segments[segment].split(t, a, b);
    segments[segment] = a;
    segments.insert(segments.begin() + static_cast<std::ptrdiff_t>(segment) + 1, b);
}

Path Path::sub_path(float d0, float d1, float tolerance) const {
    Path out;
    if (segments.empty()) return out;
    PathSampler sampler(*this, tolerance);
    const float total = sampler.length();
    d0 = clampf(d0, 0, total);
    d1 = clampf(d1, 0, total);
    if (d1 <= d0 + 1e-5f) return out;
    if (d0 <= 0 && d1 >= total) {
        out = *this;
        out.closed = false;
        return out;
    }
    const PathPoint a = sampler.at(d0), b = sampler.at(d1);
    Path head, tail, cut, rest;
    Path open = *this;
    open.closed = false;
    open.split(b.segment, b.t, cut, rest);
    if (a.segment < cut.segments.size()) {
        cut.split(a.segment, a.t, head, tail);
        return tail;
    }
    return cut;
}

void Path::join(const Path& other) {
    if (other.segments.empty()) return;
    if (segments.empty()) {
        *this = other;
        return;
    }
    if (distance(end(), other.start()) > kWeld) segments.push_back(CubicBezier::line(end(), other.start()));
    segments.insert(segments.end(), other.segments.begin(), other.segments.end());
    closed = false;
}

Path Path::reversed() const {
    Path r;
    r.closed = closed;
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) r.segments.push_back(it->reversed());
    return r;
}

void Path::transform(const Mat4& m) {
    auto tp = [&](Vec2 p) {
        const Vec3 r = m.transform_point({p.x, p.y, 0});
        return Vec2{r.x, r.y};
    };
    for (CubicBezier& s : segments) {
        s.p0 = tp(s.p0);
        s.p1 = tp(s.p1);
        s.p2 = tp(s.p2);
        s.p3 = tp(s.p3);
    }
}

void Path::flatten(float tolerance, std::vector<Vec2>& out, bool skip_first) const {
    if (segments.empty()) return;
    if (tolerance <= 0) tolerance = 0.01f;
    if (!skip_first) out.push_back(segments.front().p0);
    for (const CubicBezier& s : segments)
        flatten_recursive(s, 0, 1, tolerance, 0, [&](float, Vec2 p) { out.push_back(p); });
    if (closed && out.size() > 1 && distance(out.front(), out.back()) < kWeld) out.pop_back();
}

std::vector<Vec2> Path::offset(float dist, float tolerance, float miter_limit) const {
    std::vector<Vec2> flat;
    flatten(tolerance, flat);
    return offset_polyline(flat, dist, closed, miter_limit);
}

// ---- PathSampler -----------------------------------------------------------------------

void PathSampler::build(const Path& path, float tolerance) {
    samples_.clear();
    segments_ = path.segments;
    closed_ = path.closed;
    if (segments_.empty()) return;
    if (tolerance <= 0) tolerance = 0.01f;
    samples_.push_back({segments_.front().p0, 0, 0, 0});
    for (usize i = 0; i < segments_.size(); ++i) {
        flatten_recursive(segments_[i], 0, 1, tolerance, 0, [&](float t, Vec2 p) {
            const Sample& prev = samples_.back();
            samples_.push_back({p, prev.distance + distance(prev.position, p), i, t});
        });
    }
}

PathPoint PathSampler::at(float dist) const {
    PathPoint r;
    if (samples_.empty()) return r;
    const float total = PathSampler::length();
    if (closed_ && total > 0) {
        dist = std::fmod(dist, total);
        if (dist < 0) dist += total;
    } else {
        dist = clampf(dist, 0, total);
    }
    // First sample with distance >= dist.
    auto it = std::lower_bound(samples_.begin(), samples_.end(), dist,
                               [](const Sample& s, float d) { return s.distance < d; });
    usize hi = static_cast<usize>(it - samples_.begin());
    if (hi == 0) hi = 1;
    if (hi >= samples_.size()) hi = samples_.size() - 1;
    const Sample& a = samples_[hi - 1];
    const Sample& b = samples_[hi];
    const float span = b.distance - a.distance;
    const float u = span > 1e-8f ? clampf((dist - a.distance) / span, 0, 1) : 0;
    // Samples bracketing a segment boundary: a is the end of the previous
    // segment (t = 1), which equals the start of b's segment (t = 0).
    const float ta = a.segment == b.segment ? a.t : 0.0f;
    const CubicBezier& seg = segments_[b.segment];
    r.segment = b.segment;
    r.t = ta + (b.t - ta) * u;
    r.position = seg.point(r.t);
    r.direction = seg.direction(r.t);
    r.distance = dist;
    return r;
}

PathPoint PathSampler::closest(Vec2 p) const {
    PathPoint r;
    if (samples_.empty()) return r;
    if (samples_.size() == 1) {
        r.position = samples_[0].position;
        r.direction = segments_[0].direction(0);
        return r;
    }
    float best = 1e30f;
    usize best_i = 0;
    float best_u = 0;
    for (usize i = 0; i + 1 < samples_.size(); ++i) {
        float u;
        const Vec2 q = closest_point_on_segment(samples_[i].position, samples_[i + 1].position, p, &u);
        const float d = length_sq(q - p);
        if (d < best) {
            best = d;
            best_i = i;
            best_u = u;
        }
    }
    const Sample& a = samples_[best_i];
    const Sample& b = samples_[best_i + 1];
    const CubicBezier& seg = segments_[b.segment];
    const float ta = a.segment == b.segment ? a.t : 0.0f;
    float t = ta + (b.t - ta) * best_u;
    // Newton refinement on the exact curve: minimise |B(t) - p|^2.
    for (int i = 0; i < 6; ++i) {
        const Vec2 d = seg.point(t) - p;
        const Vec2 d1 = seg.derivative(t);
        const Vec2 d2 = second_derivative(seg, t);
        const float f = dot(d, d1);
        const float fp = dot(d1, d1) + dot(d, d2);
        if (absf(fp) < 1e-8f) break;
        const float next = clampf(t - f / fp, 0, 1);
        if (absf(next - t) < 1e-6f) {
            t = next;
            break;
        }
        t = next;
    }
    // Keep the refinement only if it actually got closer (Newton can wander on tight curves).
    const Vec2 refined = seg.point(t);
    const Vec2 coarse = lerp(a.position, b.position, best_u);
    if (length_sq(refined - p) > length_sq(coarse - p)) t = ta + (b.t - ta) * best_u;
    r.segment = b.segment;
    r.t = t;
    r.position = seg.point(t);
    r.direction = seg.direction(t);
    const float u = (b.t - ta) > 1e-8f ? clampf((t - ta) / (b.t - ta), 0, 1) : best_u;
    r.distance = a.distance + (b.distance - a.distance) * u;
    return r;
}

std::vector<Vec2> PathSampler::points() const {
    std::vector<Vec2> out;
    out.reserve(samples_.size());
    for (const Sample& s : samples_) out.push_back(s.position);
    return out;
}

// ---- polylines and polygons ------------------------------------------------------------

Vec2 closest_point_on_segment(Vec2 a, Vec2 b, Vec2 p, float* t_out) {
    const Vec2 ab = b - a;
    const float l2 = length_sq(ab);
    float t = l2 > 1e-12f ? clampf(dot(p - a, ab) / l2, 0, 1) : 0;
    if (t_out) *t_out = t;
    return a + ab * t;
}

bool line_intersection(Vec2 a, Vec2 ad, Vec2 b, Vec2 bd, Vec2& out) {
    const float denom = cross(ad, bd);
    if (absf(denom) < 1e-9f) return false;
    const float t = cross(b - a, bd) / denom;
    out = a + ad * t;
    return true;
}

bool segments_intersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Vec2* where) {
    const Vec2 r = b - a, s = d - c;
    const float denom = cross(r, s);
    const Vec2 qp = c - a;
    if (absf(denom) < 1e-9f) {
        if (absf(cross(qp, r)) > 1e-6f) return false; // parallel, not collinear
        // Collinear: overlap test along r.
        const float rr = length_sq(r);
        if (rr < 1e-12f) return false;
        const float t0 = dot(qp, r) / rr, t1 = t0 + dot(s, r) / rr;
        const bool overlap = std::max(std::min(t0, t1), 0.0f) <= std::min(std::max(t0, t1), 1.0f);
        if (overlap && where) *where = a + r * clampf(std::min(t0, t1), 0, 1);
        return overlap;
    }
    const float t = cross(qp, s) / denom;
    const float u = cross(qp, r) / denom;
    if (t < 0 || t > 1 || u < 0 || u > 1) return false;
    if (where) *where = a + r * t;
    return true;
}

std::vector<Vec2> offset_polyline(const std::vector<Vec2>& in, float dist, bool closed, float miter_limit) {
    std::vector<Vec2> pts;
    for (const Vec2& p : in)
        if (pts.empty() || distance(p, pts.back()) > kWeld) pts.push_back(p);
    if (closed && pts.size() > 2 && distance(pts.front(), pts.back()) <= kWeld) pts.pop_back();
    std::vector<Vec2> out;
    const usize n = pts.size();
    if (n < 2) return out;
    if (closed && n < 3) closed = false;
    const usize seg_count = closed ? n : n - 1;
    std::vector<Vec2> dirs(seg_count);
    for (usize i = 0; i < seg_count; ++i) dirs[i] = normalize(pts[(i + 1) % n] - pts[i]);
    const float limit = miter_limit * absf(dist);

    for (usize i = 0; i < n; ++i) {
        const bool has_prev = closed || i > 0;
        const bool has_next = closed || i + 1 < n;
        const Vec2 p = pts[i];
        if (!has_prev) {
            out.push_back(p + perp(dirs[0]) * dist);
            continue;
        }
        if (!has_next) {
            out.push_back(p + perp(dirs[seg_count - 1]) * dist);
            continue;
        }
        const Vec2 d_in = dirs[(i + seg_count - 1) % seg_count], d_out = dirs[i % seg_count];
        const Vec2 a = p + perp(d_in) * dist, b = p + perp(d_out) * dist;
        Vec2 x;
        if (dot(d_in, d_out) > 0.9999f || !line_intersection(a, d_in, b, d_out, x)) {
            out.push_back(a); // straight on (or reversal): no corner to build
            continue;
        }
        if (distance(x, p) <= limit) {
            out.push_back(x);
        } else {
            out.push_back(a);
            out.push_back(b);
        }
    }
    return out;
}

namespace {
void douglas_peucker(const std::vector<Vec2>& pts, usize first, usize last, float tol, std::vector<bool>& keep) {
    if (last <= first + 1) return;
    float worst = 0;
    usize index = first;
    for (usize i = first + 1; i < last; ++i) {
        const float d = distance(closest_point_on_segment(pts[first], pts[last], pts[i]), pts[i]);
        if (d > worst) {
            worst = d;
            index = i;
        }
    }
    if (worst > tol) {
        keep[index] = true;
        douglas_peucker(pts, first, index, tol, keep);
        douglas_peucker(pts, index, last, tol, keep);
    }
}
} // namespace

// ---- curve fitting (Schneider, Graphics Gems 1990) ---------------------------------------

namespace {

struct Fitter {
    const std::vector<Vec2>& d;
    float error_sq;
    std::vector<CubicBezier> out;

    static float b0(float u) { const float t = 1 - u; return t * t * t; }
    static float b1(float u) { const float t = 1 - u; return 3 * u * t * t; }
    static float b2(float u) { const float t = 1 - u; return 3 * u * u * t; }
    static float b3(float u) { return u * u * u; }

    Vec2 left_tangent(usize end) const { return normalize(d[end + 1] - d[end]); }
    Vec2 right_tangent(usize end) const { return normalize(d[end - 1] - d[end]); }
    Vec2 center_tangent(usize c) const {
        const Vec2 v1 = d[c - 1] - d[c], v2 = d[c] - d[c + 1];
        Vec2 t = normalize(Vec2{(v1.x + v2.x) * 0.5f, (v1.y + v2.y) * 0.5f});
        if (length_sq(t) < 1e-12f) t = normalize(v1);
        return t;
    }

    std::vector<float> chord_parameterize(usize first, usize last) const {
        std::vector<float> u(last - first + 1, 0.0f);
        for (usize i = first + 1; i <= last; ++i) u[i - first] = u[i - first - 1] + distance(d[i], d[i - 1]);
        const float total = u.back();
        for (float& v : u) v = total > 0 ? v / total : 0;
        return u;
    }

    CubicBezier generate(usize first, usize last, const std::vector<float>& u, Vec2 t1, Vec2 t2) const {
        float c00 = 0, c01 = 0, c11 = 0, x0 = 0, x1 = 0;
        for (usize i = 0; i <= last - first; ++i) {
            const Vec2 a0 = t1 * b1(u[i]), a1 = t2 * b2(u[i]);
            c00 += dot(a0, a0);
            c01 += dot(a0, a1);
            c11 += dot(a1, a1);
            const Vec2 tmp = d[first + i] - (d[first] * (b0(u[i]) + b1(u[i])) + d[last] * (b2(u[i]) + b3(u[i])));
            x0 += dot(a0, tmp);
            x1 += dot(a1, tmp);
        }
        const float det_c = c00 * c11 - c01 * c01;
        const float det_c0x = c00 * x1 - c01 * x0;
        const float det_xc1 = x0 * c11 - x1 * c01;
        float alpha_l = det_c == 0 ? 0 : det_xc1 / det_c;
        float alpha_r = det_c == 0 ? 0 : det_c0x / det_c;
        const float seg_len = distance(d[first], d[last]);
        const float epsilon = 1e-6f * seg_len;
        if (alpha_l < epsilon || alpha_r < epsilon) alpha_l = alpha_r = seg_len / 3; // Wu/Barsky heuristic
        CubicBezier b;
        b.p0 = d[first];
        b.p3 = d[last];
        b.p1 = b.p0 + t1 * alpha_l;
        b.p2 = b.p3 + t2 * alpha_r;
        return b;
    }

    void reparameterize(usize first, usize last, std::vector<float>& u, const CubicBezier& b) const {
        for (usize i = 0; i <= last - first; ++i) {
            const float t = u[i];
            const Vec2 q = b.point(t), q1 = b.derivative(t);
            // Second derivative of a cubic.
            const Vec2 q2 = ((b.p2 - b.p1 * 2.0f + b.p0) * (1 - t) + (b.p3 - b.p2 * 2.0f + b.p1) * t) * 6.0f;
            const Vec2 diff = q - d[first + i];
            const float numerator = dot(diff, q1);
            const float denominator = dot(q1, q1) + dot(diff, q2);
            if (denominator != 0) u[i] = std::clamp(t - numerator / denominator, 0.0f, 1.0f);
        }
    }

    float max_error(usize first, usize last, const CubicBezier& b, const std::vector<float>& u, usize& split) const {
        split = (last - first + 1) / 2 + first;
        float worst = 0;
        for (usize i = first + 1; i < last; ++i) {
            const float e = length_sq(b.point(u[i - first]) - d[i]);
            if (e >= worst) {
                worst = e;
                split = i;
            }
        }
        return worst;
    }

    void fit(usize first, usize last, Vec2 t1, Vec2 t2) {
        if (last - first + 1 == 2) {
            const float dist = distance(d[first], d[last]) / 3;
            out.push_back(CubicBezier{d[first], d[first] + t1 * dist, d[last] + t2 * dist, d[last]});
            return;
        }
        std::vector<float> u = chord_parameterize(first, last);
        CubicBezier b = generate(first, last, u, t1, t2);
        usize split = 0;
        float err = max_error(first, last, b, u, split);
        if (err < error_sq) {
            out.push_back(b);
            return;
        }
        if (err < error_sq * 4) {
            for (int iteration = 0; iteration < 4; ++iteration) {
                reparameterize(first, last, u, b);
                b = generate(first, last, u, t1, t2);
                err = max_error(first, last, b, u, split);
                if (err < error_sq) {
                    out.push_back(b);
                    return;
                }
            }
        }
        const Vec2 tc = center_tangent(split);
        fit(first, split, t1, tc);
        fit(split, last, tc * -1.0f, t2);
    }
};

} // namespace

Path fit_path(const std::vector<Vec2>& points, float tolerance, bool closed) {
    std::vector<Vec2> d;
    d.reserve(points.size() + 1);
    for (const Vec2& p : points)
        if (d.empty() || distance(p, d.back()) > 1e-5f) d.push_back(p);
    if (closed && d.size() > 2 && distance(d.front(), d.back()) > 1e-5f) d.push_back(d.front());
    Path path;
    if (d.size() < 2) return path;
    Fitter f{d, std::max(tolerance, 1e-4f) * std::max(tolerance, 1e-4f), {}};
    f.fit(0, d.size() - 1, f.left_tangent(0), f.right_tangent(d.size() - 1));
    path.segments = std::move(f.out);
    path.closed = closed && d.size() > 2;
    return path;
}

std::vector<Vec2> simplify_polyline(const std::vector<Vec2>& pts, float tolerance) {
    if (pts.size() < 3) return pts;
    std::vector<bool> keep(pts.size(), false);
    keep.front() = keep.back() = true;
    douglas_peucker(pts, 0, pts.size() - 1, tolerance, keep);
    std::vector<Vec2> out;
    for (usize i = 0; i < pts.size(); ++i)
        if (keep[i]) out.push_back(pts[i]);
    return out;
}

std::vector<Vec2> resample_polyline(const std::vector<Vec2>& pts, float spacing, bool closed) {
    std::vector<Vec2> out;
    if (pts.empty() || spacing <= 0) return out;
    out.push_back(pts[0]);
    const usize n = pts.size();
    const usize seg_count = closed ? n : n - 1;
    float carry = spacing; // distance until the next sample
    for (usize i = 0; i < seg_count; ++i) {
        const Vec2 a = pts[i], b = pts[(i + 1) % n];
        const float len = distance(a, b);
        if (len <= 0) continue;
        const Vec2 dir = (b - a) / len;
        float pos = carry;
        while (pos <= len) {
            out.push_back(a + dir * pos);
            pos += spacing;
        }
        carry = pos - len;
    }
    if (!closed && distance(out.back(), pts.back()) > spacing * 0.5f) out.push_back(pts.back());
    return out;
}

bool polygon_contains(const std::vector<Vec2>& poly, Vec2 p) {
    bool inside = false;
    const usize n = poly.size();
    for (usize i = 0, j = n - 1; i < n; j = i++) {
        const Vec2 a = poly[i], b = poly[j];
        if ((a.y > p.y) != (b.y > p.y)) {
            const float x = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
            if (p.x < x) inside = !inside;
        }
    }
    return inside;
}

std::vector<Vec2> convex_hull(std::vector<Vec2> pts) {
    std::sort(pts.begin(), pts.end(), [](Vec2 a, Vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3) return pts;
    std::vector<Vec2> hull(pts.size() * 2);
    usize k = 0;
    for (usize i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross(hull[k - 1] - hull[k - 2], pts[i] - hull[k - 2]) <= 0) --k;
        hull[k++] = pts[i];
    }
    for (usize i = pts.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(hull[k - 1] - hull[k - 2], pts[i - 1] - hull[k - 2]) <= 0) --k;
        hull[k++] = pts[i - 1];
    }
    hull.resize(k - 1);
    return hull;
}

} // namespace rw
