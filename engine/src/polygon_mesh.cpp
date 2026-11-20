// Triangles from areas. Kept apart from mesh.cpp so the triangulator's
// header is compiled in one place only.

#include "rewind/core/polygon.h"
#include "rewind/render/mesh.h"

#include <mapbox/earcut.hpp>

#include <array>

namespace mapbox::util {

template <> struct nth<0, rw::Vec2> {
    static float get(const rw::Vec2& p) { return p.x; }
};
template <> struct nth<1, rw::Vec2> {
    static float get(const rw::Vec2& p) { return p.y; }
};

} // namespace mapbox::util

namespace rw {

void build_polygon_set(const PolygonSet& set, float r, float g, float b, float a, Mesh& out, float uv_scale) {
    std::vector<Ring> rings;
    for (const Polygon& poly : set.polygons) {
        if (poly.outer.size() < 3) continue;
        // The triangulator wants the boundary first and the holes after it,
        // and numbers the points in exactly that order.
        rings.clear();
        rings.push_back(poly.outer);
        for (const Ring& hole : poly.holes)
            if (hole.size() >= 3) rings.push_back(hole);
        const std::vector<u32> indices = mapbox::earcut<u32>(rings);
        if (indices.size() < 3) continue;
        const u32 base = static_cast<u32>(out.vertices.size());
        for (const Ring& ring : rings)
            for (const Vec2& p : ring) out.add_vertex({p.x, p.y, 0}, {p.x * uv_scale, p.y * uv_scale}, r, g, b, a);
        for (usize i = 0; i + 2 < indices.size(); i += 3)
            out.add_triangle(base + indices[i], base + indices[i + 1], base + indices[i + 2]);
    }
}

void build_polygon_outline(const PolygonSet& set, const StrokeStyle& style, Mesh& out) {
    StrokeStyle closed = style;
    closed.closed = true;
    for (const Polygon& poly : set.polygons) {
        if (poly.outer.size() >= 2) build_polyline(poly.outer, closed, out);
        for (const Ring& hole : poly.holes)
            if (hole.size() >= 2) build_polyline(hole, closed, out);
    }
}

} // namespace rw
