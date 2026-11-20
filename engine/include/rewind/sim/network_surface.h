#pragma once

// The ground a network occupies, as one clean partition.
//
// Every track and every connector sweeps a band as wide as itself. Unite
// the bands and you have exactly where things travel. Close that up by a
// corner radius and you have the paved outline: one boundary, inward corners
// rounded, hairline gaps gone. Whatever the travelled bands enclose without
// covering is either big enough to be an island, or it is paved-over space
// nobody drives on: the hatched areas at a nose, between turn lanes, in the
// throat of a wide junction.
//
// Nothing here is stacked on anything else. Each point of the plane is in at
// most one of `paved` and `islands`, and `keep_clear` is the part of `paved`
// that no band sweeps. That holds for a crossroads, a five-way at odd
// angles, a slip lane or a roundabout alike, because none of them is a
// special case: they are all just bands.
//
// Roads, rails with their ballast, footpaths, canals: the network does not
// know which, and neither does this. Pure geometry on core/polygon.h, the
// same on every machine, so it may run in tick(). Turn the results into
// triangles with build_polygon_set(), and into kerbs with polygon_border().

#include "rewind/core/polygon.h"
#include "rewind/sim/network.h"

#include <functional>

namespace rw {

struct SurfaceOptions {
    /// Curves are flattened to within this of their true shape.
    float tolerance = 0.05f;
    /// Inward corners of the outline are rounded to this, and gaps narrower
    /// than twice it are paved over. The kerb radius of a junction corner.
    float corner_radius = 1.5f;
    /// An enclosed gap becomes an island only when it is at least this big
    /// and a circle of at least `min_island_width` across fits inside it.
    /// Anything smaller is paved and offered as keep-clear instead.
    float min_island_area = 6;
    float min_island_width = 1.5f;
    /// Unswept paving is reported as keep-clear only where it is at least
    /// this wide and this big: the crescent left in a rounded corner is not.
    float min_keep_clear_width = 0.6f;
    float min_keep_clear_area = 1.0f;
    /// Added to the width of every band, so neighbouring lanes overlap by a
    /// hair instead of leaving a seam where their curves were rounded apart.
    float seam = 0.02f;
    /// Extra width on every band beyond its track: a hard strip, a verge.
    float widen = 0;
    /// Leave tracks out (and every connector that starts or ends on one):
    /// pave the carriageway and the cycle path as two surfaces by calling
    /// twice with two filters. Empty = every track.
    std::function<bool(const CurveNetwork&, TrackRef)> include;
};

struct NetworkSurface {
    PolygonSet lanes;      ///< exactly what the tracks and connectors sweep
    PolygonSet paved;      ///< the lanes closed up: one outline, corners rounded, small gaps filled
    PolygonSet islands;    ///< enclosed gaps big enough to stand on; not part of `paved`
    PolygonSet keep_clear; ///< inside `paved`, swept by no lane, wide enough to mark

    bool empty() const { return paved.empty(); }
};

/// The surface of a whole network, or of the part of it that touches
/// `region`: every track and connector whose bounds reach into the region is
/// swept in full, so the result is right inside the region and simply stops
/// somewhere beyond it. A game with a big map builds one region per tile and
/// cuts each to its tile with polygon_intersection().
NetworkSurface network_surface(const CurveNetwork& net, const SurfaceOptions& options = {},
                               const Rect* region = nullptr);

/// The surface of one junction: its connectors and the last `reach` units of
/// every track that meets there. The ends of those stubs are cut square, so
/// this is for looking at a junction on its own (an editor preview, a
/// thumbnail), not for tiling a map.
NetworkSurface node_surface(const CurveNetwork& net, u32 node, float reach = 12, const SurfaceOptions& options = {});

/// One side of a track as a line: `side` -1 is its left edge, +1 its right,
/// 0 its centre, in its direction of travel. `inset` moves the line towards
/// the centre. This is where a lane line or an edge line goes.
std::vector<Vec2> track_edge(const CurveNetwork& net, TrackRef track, float side, float inset = 0,
                             float tolerance = 0.05f);
/// The same for a connector.
std::vector<Vec2> connector_edge(const CurveNetwork& net, u32 connector, float side, float inset = 0,
                                 float tolerance = 0.05f);

} // namespace rw
