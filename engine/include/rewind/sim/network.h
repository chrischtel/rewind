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
//
// Which track may continue into which is derived too, by default: every
// arriving track connects to every leaving one that the connect filter lets
// through. A node can instead be authored: it then carries its own list of
// turns, each with a group (a signal group, a phase, a priority class: the
// game's meaning) and the network builds exactly those. That is what a
// junction editor writes into, and it is plain reflected state, so it saves,
// rewinds and survives a hot reload like everything else.

#include "rewind/core/curve.h"
#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/sim/graph.h"
#include "rewind/sim/reflect.h"

#include <functional>
#include <vector>

namespace rw {

/// A track on an edge.
struct TrackRef {
    u32 edge = 0;
    u32 track = 0;
    bool operator==(const TrackRef&) const = default;

    void reflect(Visitor& v) {
        v.field("edge", edge);
        v.field("track", track);
    }
};

/// One authored connection at a node: the track `from`, arriving, may carry
/// on into the track `to`, leaving. The rest is the game's to give meaning.
struct Turn {
    TrackRef from;
    TrackRef to;
    u32 group = 0;    ///< which movements belong together: a signal group, a phase
    u32 tag = 0;
    i32 priority = 0; ///< higher goes first where two turns conflict

    void reflect(Visitor& v) {
        v.field("from", from);
        v.field("to", to);
        v.field("group", group);
        v.field("tag", tag);
        v.field("priority", priority);
    }
};

struct NetNode {
    Vec2 pos;
    float elevation = 0;
    float radius = 0; ///< tracks stop this far from the node, leaving room for the connectors to cross
    u32 tag = 0;
    bool alive = true;
    /// Authored nodes build exactly the connectors in `turns`; the others
    /// derive theirs through the connect filter. See author_node().
    bool authored = false;
    std::vector<Turn> turns;

    void reflect(Visitor& v) {
        v.field("pos", pos);
        v.field("elevation", elevation);
        v.field("radius", radius, {.min = 0});
        v.field("tag", tag);
        v.field("alive", alive);
        v.field("authored", authored);
        v.field("turns", turns);
    }
};

/// One track on an edge. `offset` is measured along perp() of the
/// centreline (right of travel in y-down space); `forward` tracks run from
/// the edge's node a to b, others the opposite way.
///
/// A track need not run the whole edge. `begin` and `end` cut it short of
/// node a and node b: a turn pocket that opens sixty units before a junction,
/// a lane that is dropped half way, a lay-by in the middle. A track that
/// stops short of a node has no connectors there; whoever is on it gets on
/// and off sideways, over the links to its neighbours. `taper` is the length
/// over which such an open end widens from nothing to the full width, which
/// is what the surface draws and what tells a driver the lane is ending.
struct Track {
    float offset = 0;
    bool forward = true;
    float width = 1;
    u32 tag = 0;
    float begin = 0; ///< missing for this far from node a, measured along the edge (0 = from the node)
    float end = 0;   ///< missing for this far before node b (0 = to the node)
    float taper = 0; ///< an open end widens over this length

    bool whole() const { return begin <= 0 && end <= 0; }

    void reflect(Visitor& v) {
        v.field("offset", offset);
        v.field("forward", forward);
        v.field("width", width, {.min = 0});
        v.field("tag", tag);
        v.field("begin", begin, {.min = 0});
        v.field("end", end, {.min = 0});
        v.field("taper", taper, {.min = 0});
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

/// A place along a track.
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
    u32 group = 0;    ///< from the authored turn; 0 on derived connectors
    u32 tag = 0;
    i32 priority = 0;
};

/// Derived sideways link between two parallel tracks on one edge.
struct TrackLink {
    TrackRef from;
    TrackRef to;
};

enum class ConflictKind : u8 {
    Crossing, ///< the two curves cross
    Merge,    ///< they end on the same track (only listed when merge_conflicts is on)
};

/// Two connectors at one node whose curves cross: whoever uses them has to
/// agree on an order. The game decides how (priority, signals, first come).
struct Conflict {
    u32 node = 0;
    u32 connector_a = 0;
    u32 connector_b = 0;
    Vec2 where;
    ConflictKind kind = ConflictKind::Crossing;
    float at_a = 0; ///< how far along connector a the meeting point lies
    float at_b = 0; ///< and along connector b: enough to hold short of the point, not of the node
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

/// What the last rebuild() had to do.
struct RebuildStats {
    u32 edges = 0;          ///< alive
    u32 edges_rebuilt = 0;  ///< whose tracks were worked out again
    u32 nodes = 0;
    u32 nodes_rebuilt = 0;  ///< whose connectors and conflicts were worked out again
    bool everything = false;
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
    void set_connect_filter(ConnectFilter filter) {
        filter_ = std::move(filter);
        invalidate();
    }
    /// How far short of a node the tracks of an edge stop, leaving room for
    /// the connectors. Default: the node's radius, the same for every arm.
    using TrackTrim = std::function<float(const CurveNetwork&, u32 node, u32 edge)>;
    void set_track_trim(TrackTrim trim) {
        trim_ = std::move(trim);
        invalidate();
    }

    // ---- authored junctions ----------------------------------------------------
    /// Take a node over: the connectors the network derives there right now
    /// become its authored turns, and from then on only its turns are built.
    /// Nothing changes on the ground until a turn is edited. No effect on a
    /// node that is authored already.
    void author_node(u32 node);
    /// Hand a node back: its turns are dropped and derived again.
    void release_node(u32 node);
    /// Allow or forbid one movement at a node, authoring the node first if it
    /// was not. False when `from` does not arrive there or `to` does not leave.
    bool allow_turn(u32 node, TrackRef from, TrackRef to, bool allowed = true);
    /// The authored turn for a movement, or nullptr. Edit its group, tag and
    /// priority in place.
    Turn* find_turn(u32 node, TrackRef from, TrackRef to);
    const Turn* find_turn(u32 node, TrackRef from, TrackRef to) const;
    /// Whether a track runs into a node, or out of it.
    bool arrives_at(TrackRef track, u32 node) const;
    bool leaves_from(TrackRef track, u32 node) const;
    /// Also list pairs of connectors that end on the same track as conflicts
    /// (kind Merge). Off by default: a game that orders merges by gaps along
    /// the track does not want them twice.
    bool merge_conflicts = false;
    /// Extra cost added to sideways track changes when routing.
    float change_penalty = 4;
    /// How closely a track follows the true curve beside its edge's centre
    /// line, in the network's own units. The default suits a network drawn
    /// in pixels; one in metres whose tracks are also drawn (as road surface,
    /// as lane lines) wants a few centimetres, or tight bends show corners.
    float track_tolerance = 0.5f;
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
    /// Recompute the derived data after any edit.
    ///
    /// Only what an edit could have changed is worked out again: the tracks
    /// of edges that changed or touch a node that did, and the connectors
    /// and conflicts of the nodes at their ends. Everything else is carried
    /// over, so on a large network an edit costs what its neighbourhood
    /// costs. Changes are found by comparing every node and edge with what it
    /// was at the last rebuild, so it does not matter how an edit was made:
    /// through the calls above, through node() and edge(), or by loading.
    ///
    /// The connect filter and the track trim are trusted to look only at the
    /// node they are asked about and the edges meeting there. A rule that
    /// reads further afield, or whose own logic changed, needs invalidate().
    void rebuild();
    /// Make the next rebuild() work everything out afresh.
    void invalidate() { all_dirty_ = true; }
    const RebuildStats& last_rebuild() const { return stats_; }
    void clear();

    // ---- access ------------------------------------------------------------
    const std::vector<NetNode>& nodes() const { return nodes_; }
    const std::vector<NetEdge>& edges() const { return edges_; }
    NetNode& node(u32 i) { return nodes_[i]; }
    const NetNode& node(u32 i) const { return nodes_[i]; }
    NetEdge& edge(u32 i) {
        adjacency_valid_ = false; // the caller may be about to rewire it
        return edges_[i];
    }
    const NetEdge& edge(u32 i) const { return edges_[i]; }
    bool valid_node(u32 i) const { return i < nodes_.size() && nodes_[i].alive; }
    bool valid_edge(u32 i) const { return i < edges_.size() && edges_[i].alive; }
    std::vector<u32> edges_at(u32 node) const;
    const std::vector<Connector>& connectors() const { return connectors_; }
    const std::vector<TrackLink>& links() const { return links_; }
    const std::vector<Conflict>& conflicts() const { return conflicts_; }
    /// These three are lookups into tables made by rebuild(): cheap to call.
    const std::vector<u32>& connectors_at(u32 node) const;
    /// Connectors leaving the end of a track.
    const std::vector<u32>& connectors_from(TrackRef track) const;
    /// Connectors arriving at the start of a track.
    const std::vector<u32>& connectors_into(TrackRef track) const;
    /// The connector between two tracks, or ~0u.
    u32 find_connector(TrackRef from, TrackRef to) const;
    /// Indices into conflicts() of every conflict a connector takes part in.
    const std::vector<u32>& conflicts_of(u32 connector) const;

    // ---- geometry ------------------------------------------------------------
    /// The track's curve in its direction of travel (derived, cached by rebuild()).
    const Path& track_path(TrackRef track) const;
    float track_length(TrackRef track) const;
    /// How far along the edge, in the track's own direction of travel, the
    /// track begins: the trim at the node it leaves, or its own `begin`/`end`
    /// when it starts further in. Two tracks on one edge that run the same
    /// way share this scale, which is how a place on one is found on the other.
    float track_start(TrackRef track) const;
    /// The place on `to` beside `distance` on `from` (same edge, same
    /// direction). False when `to` does not exist there.
    bool beside(TrackRef from, float distance, TrackRef to, float& out) const;
    /// Elevation along an edge, interpolated between its nodes (0..1 along the centreline).
    float elevation_at(u32 edge, float t) const;
    /// Rise over run, as a fraction: 0.018 is the 1.8% a railway would paint
    /// on a gradient post. Elevation is linear between the two nodes and is in
    /// the same units as x and y, so this is one dimensionless number for the
    /// whole edge. Positive climbs from a to b.
    float grade(u32 edge) const;
    /// The same in the direction a track is travelled, so a backward track on
    /// a climbing edge comes back as a descent. Connectors are flat by
    /// construction: both of their ends meet at one node.
    float grade_along(TrackRef track) const;
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
        v.field("merge_conflicts", merge_conflicts);
        v.field("track_tolerance", track_tolerance, {.min = 0.001f});
        if (v.writes()) rebuild(); // a save only reads: it must not rebuild the whole network
    }

private:
    void snap_edge(NetEdge& e);
    void build_track(u32 edge, u32 track, Path& path, float& length, float& along) const;
    void derive_connectors(u32 node, std::vector<Connector>& out);
    void find_conflicts(const std::vector<u32>& here, std::vector<Conflict>& out) const;
    void build_lookups();
    void build_links();
    void build_route_graph();
    void build_adjacency() const;
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
    std::vector<float> track_along_;       ///< per track index: see track_start()
    std::vector<Connector> connectors_;
    std::vector<TrackLink> links_;
    std::vector<Conflict> conflicts_;
    std::vector<std::vector<u32>> node_connectors_;     ///< per node
    std::vector<std::vector<u32>> track_connectors_;    ///< per track index: leaving its end
    std::vector<std::vector<u32>> track_feeders_;       ///< per track index: arriving at its start
    std::vector<std::vector<u32>> connector_conflicts_; ///< per connector
    Graph route_graph_;                    ///< nodes = tracks; edges = connectors and links
    std::vector<i32> route_edge_kind_;     ///< per graph edge: >= 0 connector index, < 0 -(link index + 1)
    std::vector<std::pair<TrackRef, float>> penalties_;
    std::vector<u64> track_changed_;       ///< per track index: version of its last penalty change
    // What every node and edge looked like at the last rebuild, to find out
    // what has changed since.
    std::vector<u64> node_sig_;
    std::vector<u64> edge_sig_;
    std::vector<u32> edge_a_, edge_b_;     ///< the ends each edge had then
    u64 config_sig_ = 0;
    bool all_dirty_ = true;
    RebuildStats stats_;
    mutable std::vector<std::vector<u32>> adjacency_; ///< per node: its alive edges, in index order
    mutable bool adjacency_valid_ = false;
    u64 version_ = 1;
    u64 rebuilt_version_ = 1;
    mutable RouteCache cache_{512};
};

} // namespace rw
