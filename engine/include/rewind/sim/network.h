#pragma once

// A curve network: nodes joined by curved edges, each edge carrying
// parallel tracks (offset from the centreline, running with or against
// it). At every node, tracks arriving connect to tracks leaving through
// connector curves with continuous tangents; parallel tracks on one edge
// link sideways. From that a routing graph is derived, so anything can be
// asked "how do I get from here to there along the tracks", and the answer
// is one Path an agent can follow.
//
// Roads with lanes, rails with tracks, rivers, pipes, belts, footpaths:
// the network does not know which. Nodes carry an elevation for bridges
// and dips; what that means visually is the game's choice.
//
// Nodes, edges and tracks are the authored data and reflect. Connectors,
// links and the routing graph are derived: rebuild() after editing.

#include "rewind/core/curve.h"
#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/sim/graph.h"
#include "rewind/sim/reflect.h"

#include <functional>
#include <vector>

namespace rw {

struct NetNode {
    Vec2 pos;
    float elevation = 0;
    float radius = 0; ///< tracks stop this far from the node, leaving room for the connectors to cross
    u32 tag = 0;
    bool alive = true;

    void reflect(Visitor& v) {
        v.field("pos", pos);
        v.field("elevation", elevation);
        v.field("radius", radius, {.min = 0});
        v.field("tag", tag);
        v.field("alive", alive);
    }
};

/// One track on an edge. `offset` is measured along perp() of the
/// centreline (right of travel in y-down space); `forward` tracks run from
/// the edge's node a to b, others the opposite way.
struct Track {
    float offset = 0;
    bool forward = true;
    float width = 1;
    u32 tag = 0;

    void reflect(Visitor& v) {
        v.field("offset", offset);
        v.field("forward", forward);
        v.field("width", width, {.min = 0});
        v.field("tag", tag);
    }
};

struct NetEdge {
    u32 a = 0;
    u32 b = 0;
    Path path;            ///< centreline from a to b; kept snapped to the nodes by rebuild()
    std::vector<Track> tracks;
    float width = 1;      ///< the whole edge, for drawing and hit testing
    u32 tag = 0;
    bool alive = true;

    void reflect(Visitor& v) {
        v.field("a", a);
        v.field("b", b);
        v.field("path", path);
        v.field("tracks", tracks);
        v.field("width", width, {.min = 0});
        v.field("tag", tag);
        v.field("alive", alive);
    }
};

/// A track on an edge, and a place along it.
struct TrackRef {
    u32 edge = 0;
    u32 track = 0;
    bool operator==(const TrackRef&) const = default;
};

struct Location {
    TrackRef track;
    float distance = 0; ///< along the track, in its direction of travel
};

/// Derived curve from the end of one track to the start of another at a node.
struct Connector {
    u32 node = 0;
    TrackRef from;
    TrackRef to;
    Path path;
    float length = 0;
};

/// Derived sideways link between two parallel tracks on one edge.
struct TrackLink {
    TrackRef from;
    TrackRef to;
};

/// Two connectors at one node whose curves cross: whoever uses them has to
/// agree on an order. The game decides how (priority, signals, first come).
struct Conflict {
    u32 node = 0;
    u32 connector_a = 0;
    u32 connector_b = 0;
    Vec2 where;
};

struct RouteStep {
    enum class Kind : u8 { Track, Connector, Change };
    Kind kind = Kind::Track;
    u32 index = 0; ///< track index (see CurveNetwork::track_index), connector index, or link index

    void reflect(Visitor& v) {
        v.field("kind", kind);
        v.field("index", index);
    }
};

struct Route {
    bool found = false;
    float cost = 0;
    u64 version = 0;              ///< network version it was planned on
    std::vector<RouteStep> steps; ///< alternating tracks and connectors/changes, starting on the start track

    void reflect(Visitor& v) {
        v.field("found", found);
        v.field("cost", cost);
        v.field("version", version);
        v.field("steps", steps);
    }
};

class CurveNetwork {
public:
    // ---- authoring -----------------------------------------------------------
    u32 add_node(Vec2 pos, float elevation = 0, u32 tag = 0, float radius = 0);
    /// Straight edge unless a path is given (its ends are snapped to the nodes).
    u32 add_edge(u32 a, u32 b, const Path* path = nullptr);
    /// Edge whose ends leave the nodes along the given directions (a cubic).
    u32 add_curved_edge(u32 a, u32 b, Vec2 dir_a, Vec2 dir_b);
    /// Remove an edge, or a node with every edge touching it.
    void remove_edge(u32 edge);
    void remove_node(u32 node);
    /// `count` tracks each way, evenly spaced across the edge's width.
    void set_tracks(u32 edge, u32 forward_count, u32 backward_count, float track_width);
    /// Cut an edge at parameter t of segment `segment` into two, returning the new node.
    u32 split_edge(u32 edge, usize segment, float t);
    /// Move a node; its edges' ends follow at the next rebuild().
    void move_node(u32 node, Vec2 pos);
    /// Whether a node may connect this incoming track to that outgoing one.
    /// Default: everything except turning straight back onto the same edge.
    using ConnectFilter = std::function<bool(const CurveNetwork&, u32 node, TrackRef from, TrackRef to)>;
    void set_connect_filter(ConnectFilter filter) { filter_ = std::move(filter); }
    /// How far short of a node the tracks of an edge stop, leaving room for
    /// the connectors. Default: the node's radius, the same for every arm.
    using TrackTrim = std::function<float(const CurveNetwork&, u32 node, u32 edge)>;
    void set_track_trim(TrackTrim trim) { trim_ = std::move(trim); }
    /// Extra cost added to sideways track changes when routing.
    float change_penalty = 4;
    /// Runtime cost added to entering a track (congestion, closure, wear).
    /// Kept across rebuild(); not reflected: set it again from your state.
    void set_track_penalty(TrackRef track, float penalty);
    float track_penalty(TrackRef track) const;
    void clear_track_penalties();
    /// Bumps on rebuild() and on every penalty change.
    u64 version() const { return version_; }
    /// True when something a route runs over changed since it was planned:
    /// the structure was rebuilt, or a penalty on one of its tracks moved.
    bool route_affected(const Route& r) const;
    /// Big graphs: meet-in-the-middle search instead of A* from one end.
    bool bidirectional = false;
    /// Recompute the derived data after any edit. Cheap enough per edit.
    void rebuild();
    void clear();

    // ---- access ------------------------------------------------------------
    const std::vector<NetNode>& nodes() const { return nodes_; }
    const std::vector<NetEdge>& edges() const { return edges_; }
    NetNode& node(u32 i) { return nodes_[i]; }
    const NetNode& node(u32 i) const { return nodes_[i]; }
    NetEdge& edge(u32 i) { return edges_[i]; }
    const NetEdge& edge(u32 i) const { return edges_[i]; }
    bool valid_node(u32 i) const { return i < nodes_.size() && nodes_[i].alive; }
    bool valid_edge(u32 i) const { return i < edges_.size() && edges_[i].alive; }
    std::vector<u32> edges_at(u32 node) const;
    const std::vector<Connector>& connectors() const { return connectors_; }
    const std::vector<TrackLink>& links() const { return links_; }
    const std::vector<Conflict>& conflicts() const { return conflicts_; }
    std::vector<u32> connectors_at(u32 node) const;
    /// Connectors leaving the end of a track.
    std::vector<u32> connectors_from(TrackRef track) const;

    // ---- geometry ------------------------------------------------------------
    /// The track's curve in its direction of travel (derived, cached by rebuild()).
    const Path& track_path(TrackRef track) const;
    float track_length(TrackRef track) const;
    /// Elevation along an edge, interpolated between its nodes (0..1 along the centreline).
    float elevation_at(u32 edge, float t) const;
    /// Position and heading at a location.
    PathPoint point_at(const Location& loc) const;
    /// Nearest node within `max_distance`, or ~0u.
    u32 nearest_node(Vec2 p, float max_distance = 1e30f) const;
    /// Nearest edge centreline within `max_distance`; `t` receives 0..1 along it.
    u32 nearest_edge(Vec2 p, float max_distance = 1e30f, float* t = nullptr) const;
    /// Nearest track (by its curve) within `max_distance`.
    bool locate(Vec2 p, Location& out, float max_distance = 1e30f) const;

    // ---- routing -------------------------------------------------------------
    /// Shortest way from one track to another over connectors and sideways
    /// changes. Costs are lengths; changes add change_penalty.
    Route route(TrackRef from, TrackRef to) const;
    Route route(const Location& from, const Location& to) const;
    /// The route as one Path, from `from.distance` on the first track to
    /// `to.distance` on the last (whole tracks when locations are omitted).
    Path route_path(const Route& r, const Location* from = nullptr, const Location* to = nullptr) const;
    /// Dense index of a track (for tables keyed by track), and back.
    u32 track_index(TrackRef track) const;
    TrackRef track_ref(u32 index) const { return index < track_refs_.size() ? track_refs_[index] : TrackRef{}; }
    usize track_count() const { return track_refs_.size(); }
    const Graph& route_graph() const { return route_graph_; }
    RouteCache& cache() { return cache_; }

    void reflect(Visitor& v) {
        v.field("nodes", nodes_);
        v.field("edges", edges_);
        v.field("change_penalty", change_penalty, {.min = 0});
        if (v.writes()) rebuild(); // a save only reads: it must not rebuild the whole network
    }

private:
    void snap_edge(NetEdge& e);
    void build_tracks();
    void build_connectors();
    void build_links();
    void build_route_graph();
    void build_conflicts();
    void apply_penalties();
    Path connector_path(TrackRef from, TrackRef to) const;

    std::vector<NetNode> nodes_;
    std::vector<NetEdge> edges_;
    ConnectFilter filter_;
    TrackTrim trim_;

    // Derived.
    std::vector<TrackRef> track_refs_;
    std::vector<u32> track_first_;         ///< per edge: index of its first track in track_refs_
    std::vector<Path> track_paths_;        ///< per track index
    std::vector<float> track_lengths_;
    std::vector<Connector> connectors_;
    std::vector<TrackLink> links_;
    std::vector<Conflict> conflicts_;
    Graph route_graph_;                    ///< nodes = tracks; edges = connectors and links
    std::vector<i32> route_edge_kind_;     ///< per graph edge: >= 0 connector index, < 0 -(link index + 1)
    std::vector<std::pair<TrackRef, float>> penalties_;
    std::vector<u64> track_changed_;       ///< per track index: version of its last penalty change
    u64 version_ = 1;
    u64 rebuilt_version_ = 1;
    mutable RouteCache cache_{512};
};

} // namespace rw
