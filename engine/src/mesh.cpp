#include "rewind/render/mesh.h"

#include <algorithm>
#include <cmath>

namespace rw {

// ---- Mesh ------------------------------------------------------------------------------

u32 Mesh::add_vertex(Vec3 pos, Vec2 uv, float r, float g, float b, float a) {
    vertices.push_back(MeshVertex{pos, uv, r, g, b, a});
    return static_cast<u32>(vertices.size() - 1);
}

void Mesh::add_triangle(u32 a, u32 b, u32 c) {
    indices.push_back(a);
    indices.push_back(b);
    indices.push_back(c);
}

void Mesh::add_quad(u32 a, u32 b, u32 c, u32 d) {
    add_triangle(a, b, c);
    add_triangle(a, c, d);
}

// ---- helpers ---------------------------------------------------------------------------

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kTau = 6.2831853f;

Vec2 sub(Vec2 a, Vec2 b) { return a - b; }
Vec2 add(Vec2 a, Vec2 b) { return a + b; }
Vec2 mul(Vec2 a, float s) { return a * s; }

/// Split a polyline into dash pieces along its length. The pattern starts
/// `dash_offset` units in, so animating the offset scrolls the dashes.
std::vector<std::vector<Vec2>> apply_dashes(const std::vector<Vec2>& pts, const StrokeStyle& style) {
    std::vector<std::vector<Vec2>> pieces;
    if (style.dash <= 0) {
        pieces.push_back(pts);
        return pieces;
    }
    const float period = style.dash + std::max(style.gap, 0.0f);
    auto phase = [&](float t) {
        float p = std::fmod(t, period);
        if (p < 0) p += period;
        return p;
    };
    float travelled = style.dash_offset;
    std::vector<Vec2> current;
    bool drawing = false;
    const usize n = pts.size();
    const usize segments = style.closed ? n : n - 1;
    for (usize i = 0; i < segments; ++i) {
        const Vec2 a = pts[i], b = pts[(i + 1) % n];
        const float seg_len = length(sub(b, a));
        if (seg_len <= 0) continue;
        const Vec2 dir = mul(sub(b, a), 1.0f / seg_len);
        float pos = 0;
        while (pos < seg_len) {
            const float p = phase(travelled + pos);
            const bool on = p < style.dash;
            const float remaining_in_phase = on ? style.dash - p : period - p;
            const float step = std::min(remaining_in_phase, seg_len - pos);
            const Vec2 from = add(a, mul(dir, pos));
            const Vec2 to = add(a, mul(dir, pos + step));
            if (on) {
                if (!drawing) {
                    current.clear();
                    current.push_back(from);
                    drawing = true;
                }
                current.push_back(to);
            } else if (drawing) {
                pieces.push_back(current);
                drawing = false;
            }
            pos += std::max(step, 1e-5f);
        }
        travelled += seg_len;
    }
    if (drawing && current.size() >= 2) pieces.push_back(current);
    return pieces;
}

struct Stroker {
    const StrokeStyle& style;
    Mesh& out;
    float hw;
    float r, g, b, a;

    u32 vertex(Vec2 p, float u, float v) { return out.add_vertex({p.x, p.y, 0}, {u, v}, r, g, b, a); }

    /// Fan around `center` from direction `from` to `to`, the short way round
    /// (so never more than a half turn; caps call this twice).
    void fan(Vec2 center, u32 center_vertex, Vec2 from, Vec2 to, float u) {
        const float a0 = std::atan2(from.y, from.x);
        float delta = std::atan2(to.y, to.x) - a0;
        while (delta > kPi) delta -= kTau;
        while (delta < -kPi) delta += kTau;
        const int steps = std::max(1, static_cast<int>(std::ceil(std::fabs(delta) / kTau * style.round_segments * 4)));
        u32 prev = vertex(add(center, mul(from, hw)), u, 0);
        for (int i = 1; i <= steps; ++i) {
            const float ang = a0 + delta * static_cast<float>(i) / static_cast<float>(steps);
            const u32 cur = vertex(add(center, Vec2{std::cos(ang) * hw, std::sin(ang) * hw}), u, 0);
            out.add_triangle(center_vertex, prev, cur);
            prev = cur;
        }
    }

    void stroke(const std::vector<Vec2>& pts, bool closed, float u_start, float uv_scale) {
        const usize n = pts.size();
        if (n < 2) return;
        const usize seg_count = closed ? n : n - 1;
        std::vector<Vec2> dirs(seg_count);
        for (usize i = 0; i < seg_count; ++i) dirs[i] = normalize(sub(pts[(i + 1) % n], pts[i]));

        // Per point: the vertex pair that ends the incoming segment and the
        // pair that starts the outgoing one. Equal except at bevel/round joins.
        struct Pair {
            u32 l, r;
        };
        std::vector<Pair> in_pair(n), out_pair(n);

        float u = u_start;
        for (usize i = 0; i < n; ++i) {
            const bool has_prev = closed || i > 0;
            const bool has_next = closed || i + 1 < n;
            const Vec2 d_in = has_prev ? dirs[(i + seg_count - 1) % seg_count] : dirs[0];
            const Vec2 d_out = has_next ? dirs[i % seg_count] : dirs[seg_count - 1];
            const Vec2 n_in = perp(d_in), n_out = perp(d_out);
            if (i > 0) u += length(sub(pts[i], pts[i - 1])) * uv_scale;
            Vec2 p = pts[i];

            if (!has_prev || !has_next) {
                if (!has_prev && style.cap == LineCap::Square) p = sub(p, mul(d_out, hw));
                if (!has_next && style.cap == LineCap::Square) p = add(p, mul(d_in, hw));
                const Vec2 nrm = has_next ? n_out : n_in;
                const Pair pair{vertex(add(p, mul(nrm, hw)), u, 0), vertex(sub(p, mul(nrm, hw)), u, 1)};
                in_pair[i] = out_pair[i] = pair;
                continue;
            }

            const float turn = cross(d_in, d_out);
            const Vec2 miter = normalize(add(n_in, n_out));
            const float cos_half = dot(miter, n_out);
            const bool straight = std::fabs(turn) < 1e-5f && dot(d_in, d_out) > 0;
            const float scale = cos_half > 1e-4f ? 1.0f / cos_half : 1e6f;

            if (straight || (style.join == LineJoin::Miter && scale <= style.miter_limit)) {
                const Vec2 off = straight ? n_out : mul(miter, scale);
                const Pair pair{vertex(add(p, mul(off, hw)), u, 0), vertex(sub(p, mul(off, hw)), u, 1)};
                in_pair[i] = out_pair[i] = pair;
                continue;
            }

            // Bevel or round join. The path turns towards the +normal side
            // when turn > 0; that side is the inner one.
            const float inner_sign = turn > 0 ? 1.0f : -1.0f;
            const float inner_v = inner_sign > 0 ? 0.0f : 1.0f;
            const float outer_v = 1.0f - inner_v;
            const Vec2 outer_in_p = sub(p, mul(n_in, inner_sign * hw));
            const Vec2 outer_out_p = sub(p, mul(n_out, inner_sign * hw));
            const u32 outer_in = vertex(outer_in_p, u, outer_v);
            const u32 outer_out = vertex(outer_out_p, u, outer_v);

            // Sharing one inner vertex avoids overlap (visible with alpha),
            // but a very sharp inner miter would shoot far away, so fall
            // back to separate inner vertices there.
            const bool share_inner = scale <= 8.0f;
            u32 inner_in, inner_out, apex;
            if (share_inner) {
                inner_in = inner_out = vertex(add(p, mul(miter, inner_sign * hw * scale)), u, inner_v);
                apex = inner_in;
            } else {
                inner_in = vertex(add(p, mul(n_in, inner_sign * hw)), u, inner_v);
                inner_out = vertex(add(p, mul(n_out, inner_sign * hw)), u, inner_v);
                apex = vertex(p, u, 0.5f);
            }
            in_pair[i] = inner_sign > 0 ? Pair{inner_in, outer_in} : Pair{outer_in, inner_in};
            out_pair[i] = inner_sign > 0 ? Pair{inner_out, outer_out} : Pair{outer_out, inner_out};

            if (style.join == LineJoin::Round) {
                // Sector from the corner point, plus the two slivers between
                // the apex, the corner and the chord ends.
                const u32 centre = vertex(p, u, 0.5f);
                fan(p, centre, normalize(sub(outer_in_p, p)), normalize(sub(outer_out_p, p)), u);
                if (apex != centre) {
                    out.add_triangle(apex, outer_in, centre);
                    out.add_triangle(apex, centre, outer_out);
                }
            } else {
                out.add_triangle(apex, outer_in, outer_out);
            }
        }

        for (usize i = 0; i < seg_count; ++i) {
            const usize j = (i + 1) % n;
            out.add_quad(out_pair[i].l, in_pair[j].l, in_pair[j].r, out_pair[i].r);
        }

        if (!closed && style.cap == LineCap::Round) {
            const Vec2 d0 = dirs[0], d1 = dirs[seg_count - 1];
            const u32 c0 = vertex(pts[0], u_start, 0.5f);
            fan(pts[0], c0, perp(d0), mul(d0, -1), u_start);
            fan(pts[0], c0, mul(d0, -1), mul(perp(d0), -1), u_start);
            const u32 c1 = vertex(pts[n - 1], u, 0.5f);
            fan(pts[n - 1], c1, mul(perp(d1), -1), d1, u);
            fan(pts[n - 1], c1, d1, perp(d1), u);
        }
    }
};

} // namespace

// ---- builders --------------------------------------------------------------------------

float polyline_length(const std::vector<Vec2>& pts, bool closed) {
    float total = 0;
    for (usize i = 0; i + 1 < pts.size(); ++i) total += length(sub(pts[i + 1], pts[i]));
    if (closed && pts.size() > 1) total += length(sub(pts[0], pts.back()));
    return total;
}

void build_polyline(const std::vector<Vec2>& points, const StrokeStyle& style, Mesh& out) {
    if (points.size() < 2 || style.width <= 0) return;
    // Drop consecutive duplicates; they break direction math.
    std::vector<Vec2> pts;
    for (const Vec2& p : points)
        if (pts.empty() || length(sub(p, pts.back())) > 1e-4f) pts.push_back(p);
    if (style.closed && pts.size() > 2 && length(sub(pts.front(), pts.back())) <= 1e-4f) pts.pop_back();
    if (pts.size() < 2) return;

    const bool closed = style.closed && pts.size() > 2;
    const float total = polyline_length(pts, closed);
    const float uv_scale = style.uv_per_unit > 0 ? style.uv_per_unit : (total > 0 ? 1.0f / total : 0);
    Stroker s{style, out, style.width * 0.5f, style.r, style.g, style.b, style.a};
    if (style.dash <= 0) {
        s.stroke(pts, closed, 0, uv_scale);
        return;
    }
    float u = 0;
    for (const std::vector<Vec2>& piece : apply_dashes(pts, style)) {
        s.stroke(piece, false, u, uv_scale);
        u += (polyline_length(piece) + style.gap) * uv_scale;
    }
}

float polygon_area(const std::vector<Vec2>& p) {
    float area = 0;
    for (usize i = 0; i < p.size(); ++i) area += cross(p[i], p[(i + 1) % p.size()]);
    return area * 0.5f;
}

void build_polygon(const std::vector<Vec2>& points, float r, float g, float b, float a, Mesh& out, float uv_scale) {
    if (points.size() < 3) return;
    std::vector<usize> idx(points.size());
    for (usize i = 0; i < idx.size(); ++i) idx[i] = i;
    const bool ccw = polygon_area(points) > 0;
    const u32 base = static_cast<u32>(out.vertices.size());
    for (const Vec2& p : points) out.add_vertex({p.x, p.y, 0}, {p.x * uv_scale, p.y * uv_scale}, r, g, b, a);

    auto is_convex = [&](usize a0, usize b0, usize c0) {
        const float cr = cross(sub(points[b0], points[a0]), sub(points[c0], points[b0]));
        return ccw ? cr > 0 : cr < 0;
    };
    auto inside = [&](Vec2 p, Vec2 a0, Vec2 b0, Vec2 c0) {
        const float d1 = cross(sub(b0, a0), sub(p, a0));
        const float d2 = cross(sub(c0, b0), sub(p, b0));
        const float d3 = cross(sub(a0, c0), sub(p, c0));
        const bool neg = d1 < 0 || d2 < 0 || d3 < 0;
        const bool pos = d1 > 0 || d2 > 0 || d3 > 0;
        return !(neg && pos);
    };

    int guard = 0;
    while (idx.size() > 3 && guard++ < 10000) {
        bool clipped = false;
        for (usize i = 0; i < idx.size(); ++i) {
            const usize ia = idx[(i + idx.size() - 1) % idx.size()], ib = idx[i], ic = idx[(i + 1) % idx.size()];
            if (!is_convex(ia, ib, ic)) continue;
            bool empty = true;
            for (usize k : idx) {
                if (k == ia || k == ib || k == ic) continue;
                if (inside(points[k], points[ia], points[ib], points[ic])) {
                    empty = false;
                    break;
                }
            }
            if (!empty) continue;
            out.add_triangle(base + static_cast<u32>(ia), base + static_cast<u32>(ib), base + static_cast<u32>(ic));
            idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) break; // degenerate input: stop rather than spin
    }
    if (idx.size() == 3)
        out.add_triangle(base + static_cast<u32>(idx[0]), base + static_cast<u32>(idx[1]), base + static_cast<u32>(idx[2]));
}

void build_circle(Vec2 c, float radius, float inner, int segments, float r, float g, float b, float a, Mesh& out) {
    if (segments < 3) segments = 3;
    if (inner <= 0) {
        const u32 center = out.add_vertex({c.x, c.y, 0}, {0.5f, 0.5f}, r, g, b, a);
        u32 first = 0, prev = 0;
        for (int i = 0; i < segments; ++i) {
            const float ang = static_cast<float>(i) / static_cast<float>(segments) * kTau;
            const u32 v = out.add_vertex({c.x + std::cos(ang) * radius, c.y + std::sin(ang) * radius, 0},
                                         {0.5f + std::cos(ang) * 0.5f, 0.5f + std::sin(ang) * 0.5f}, r, g, b, a);
            if (i == 0) first = v;
            else out.add_triangle(center, prev, v);
            prev = v;
        }
        out.add_triangle(center, prev, first);
        return;
    }
    u32 first_o = 0, first_i = 0, prev_o = 0, prev_i = 0;
    for (int i = 0; i < segments; ++i) {
        const float ang = static_cast<float>(i) / static_cast<float>(segments) * kTau;
        const float u = static_cast<float>(i) / static_cast<float>(segments);
        const u32 o = out.add_vertex({c.x + std::cos(ang) * radius, c.y + std::sin(ang) * radius, 0}, {u, 0}, r, g, b, a);
        const u32 in = out.add_vertex({c.x + std::cos(ang) * inner, c.y + std::sin(ang) * inner, 0}, {u, 1}, r, g, b, a);
        if (i == 0) {
            first_o = o;
            first_i = in;
        } else {
            out.add_quad(prev_o, o, in, prev_i);
        }
        prev_o = o;
        prev_i = in;
    }
    out.add_quad(prev_o, first_o, first_i, prev_i);
}

} // namespace rw
