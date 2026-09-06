#pragma once

#include "rewind/core/math.h"
#include "rewind/core/types.h"

#include <vector>

namespace rw {

struct Color;

/// One vertex of anything the renderer draws: position (z for 3D and for
/// depth-sorted 2D), texture coordinates, and a color the shader multiplies in.
struct MeshVertex {
    Vec3 pos;
    Vec2 uv;
    float r = 1, g = 1, b = 1, a = 1;
};

/// Triangle list. Build it once, draw it every frame, or build it every
/// frame for dynamic geometry; both are cheap.
struct Mesh {
    std::vector<MeshVertex> vertices;
    std::vector<u32> indices;

    void clear() {
        vertices.clear();
        indices.clear();
    }
    u32 add_vertex(Vec3 pos, Vec2 uv, float r, float g, float b, float a);
    void add_triangle(u32 a, u32 b, u32 c);
    void add_quad(u32 a, u32 b, u32 c, u32 d); ///< a-b-c-d in ring order
    usize triangle_count() const { return indices.size() / 3; }
};

// ---- builders: pure geometry, no GPU ------------------------------------------------

enum class LineJoin : u8 { Miter, Bevel, Round };
enum class LineCap : u8 { Butt, Square, Round };

struct StrokeStyle {
    float width = 1;
    LineJoin join = LineJoin::Miter;
    LineCap cap = LineCap::Butt;
    float miter_limit = 4;    ///< Longer miters become bevels.
    float dash = 0;           ///< Dash length; 0 = solid.
    float gap = 0;            ///< Gap between dashes.
    float dash_offset = 0;
    bool closed = false;      ///< Connect the last point to the first.
    float uv_per_unit = 0;    ///< u advances this much per length unit; 0 = u spans 0..1 over the whole line.
    float r = 1, g = 1, b = 1, a = 1;
    int round_segments = 6;   ///< Quality of round joins and caps.
};

/// Thick line through `points`. v runs 0..1 across the width, u along the
/// length, so a repeating texture draws road markings, ropes or borders.
/// Appends to `out`.
void build_polyline(const std::vector<Vec2>& points, const StrokeStyle& style, Mesh& out);

/// Filled simple polygon (no self-intersections; holes not supported yet)
/// by ear clipping, with uv = position * uv_scale. Appends to `out`.
void build_polygon(const std::vector<Vec2>& points, float r, float g, float b, float a, Mesh& out,
                   float uv_scale = 0);

/// Filled circle or ring. `inner_radius` 0 gives a disc.
void build_circle(Vec2 center, float radius, float inner_radius, int segments, float r, float g, float b, float a,
                  Mesh& out);

/// Length of a polyline.
float polyline_length(const std::vector<Vec2>& points, bool closed = false);

/// Signed area (positive when counter-clockwise in y-up, clockwise in y-down screen space).
float polygon_area(const std::vector<Vec2>& points);

} // namespace rw
