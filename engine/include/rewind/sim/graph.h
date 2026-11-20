#pragma once

// A weighted graph with shortest-path search. Nodes may carry a position
// (used as the A* heuristic and by nearest_node); edges carry a cost and
// can be one-way or disabled. Deterministic: ties break by node id, so a
// search gives the same route on every machine. Reflects, so a graph can be
// part of saved state or a scene.

#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/sim/reflect.h"

#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

namespace rw {

struct GraphNode {
    Vec2 pos;
    bool alive = true;
    u32 tag = 0; ///< yours: a tile type, an entity index, anything

    void reflect(Visitor& v) {
        v.field("pos", pos);
        v.field("alive", alive);
        v.field("tag", tag);
    }
};

struct GraphEdge {
    u32 from = 0;
    u32 to = 0;
    float cost = 1;
    bool enabled = true;

    void reflect(Visitor& v) {
        v.field("from", from);
        v.field("to", to);
        v.field("cost", cost, {.min = 0});
        v.field("enabled", enabled);
    }
};

struct GraphPath {
    std::vector<u32> nodes; ///< from start to goal inclusive; empty when not found
    float cost = 0;
    bool found = false;
};

class Graph {
public:
    static constexpr u32 kNone = ~0u;

    // ---- building ----------------------------------------------------------
    u32 add_node(Vec2 pos = {}, u32 tag = 0);
    /// Marks the node dead and disables its edges; ids stay stable.
    void remove_node(u32 node);
    /// Adds an edge (two edges when not `directed`). Returns the first edge's index.
    u32 add_edge(u32 from, u32 to, float cost, bool directed = false);
    /// Edge cost from the node positions.
    u32 connect(u32 from, u32 to, bool directed = false);
    void remove_edge(u32 edge);
    void set_edge_cost(u32 edge, float cost);
    void clear();
    /// Empty the graph but keep its memory, for one that is filled again
    /// straight away with about as much as it held: no allocation per node.
    void reset();

    /// Runtime extra on top of the authored cost (congestion, closures,
    /// wear). Not reflected: derived from the simulation, set it again on
    /// load. Searches use cost + penalty.
    void set_edge_penalty(u32 edge, float penalty);
    float edge_penalty(u32 edge) const { return edge < penalties_.size() ? penalties_[edge] : 0.0f; }
    void clear_penalties();
    float edge_cost(u32 edge) const { return edges_[edge].cost + edge_penalty(edge); }
    /// Bumps on every change to nodes, edges, costs or penalties, so cached
    /// routes can tell when they went stale.
    u64 version() const { return version_; }

    /// Regular grid of w x h nodes spaced `cell` apart, 4- or 8-connected.
    /// `passable(x, y)` (optional) leaves cells out; node id = y * w + x.
    static Graph grid(u32 w, u32 h, float cell, bool diagonals = false,
                      const std::function<bool(u32, u32)>& passable = nullptr, Vec2 origin = {});

    // ---- access ------------------------------------------------------------
    usize node_count() const { return nodes_.size(); }
    usize edge_count() const { return edges_.size(); }
    const GraphNode& node(u32 id) const { return nodes_[id]; }
    GraphNode& node(u32 id) { return nodes_[id]; }
    const GraphEdge& edge(u32 id) const { return edges_[id]; }
    bool valid(u32 id) const { return id < nodes_.size() && nodes_[id].alive; }
    /// Edge indices leaving `node` (including disabled ones).
    const std::vector<u32>& edges_from(u32 node) const;
    /// Edge indices arriving at `node` (including disabled ones).
    const std::vector<u32>& edges_into(u32 node) const;
    /// Enabled edge from a to b, or kNone.
    u32 find_edge(u32 from, u32 to) const;
    u32 nearest_node(Vec2 p, float max_distance = 1e30f) const;

    // ---- search ------------------------------------------------------------
    /// A* when nodes have positions (Euclidean heuristic scaled by `heuristic_weight`),
    /// plain Dijkstra with weight 0. Optional `passable(node)` filter.
    GraphPath shortest_path(u32 from, u32 to, float heuristic_weight = 1,
                            const std::function<bool(u32)>& passable = nullptr) const;
    /// Bidirectional Dijkstra: meets in the middle, so long routes on big
    /// graphs explore far less than shortest_path(). Same result, same
    /// tie-breaking rules.
    GraphPath bidirectional_path(u32 from, u32 to, const std::function<bool(u32)>& passable = nullptr) const;

    /// One search that answers "which way to `goal`" for every node at
    /// once: the edge to take next and the remaining cost. For crowds
    /// heading to the same place, or for re-routing without a new search.
    struct FlowField {
        u32 goal = kNone;
        u64 version = 0;
        std::vector<u32> next_edge; ///< per node: edge index to take, kNone when unreachable (or at the goal)
        std::vector<float> distance; ///< per node: cost to the goal, 1e30 when unreachable
        bool reachable(u32 node) const { return node < distance.size() && distance[node] < 1e30f; }
    };
    FlowField flow_to(u32 goal, float max_cost = 1e30f) const;

    /// Dijkstra flood: cost to reach every node from `from` (1e30 = unreachable), stopping at `max_cost`.
    std::vector<float> distances_from(u32 from, float max_cost = 1e30f) const;
    /// Nodes reachable within `max_cost`, nearest first.
    std::vector<u32> reachable(u32 from, float max_cost) const;
    /// Every path node's position, for drawing or following.
    std::vector<Vec2> positions(const GraphPath& path) const;

    void reflect(Visitor& v) {
        v.field("nodes", nodes_);
        v.field("edges", edges_);
        if (v.writes()) rebuild_adjacency();
    }

private:
    void rebuild_adjacency();

    std::vector<GraphNode> nodes_;
    std::vector<GraphEdge> edges_;
    std::vector<std::vector<u32>> out_; ///< per node: edge indices; derived
    std::vector<std::vector<u32>> in_;
    std::vector<float> penalties_;
    u64 version_ = 1;
};

/// Remembers routes by (from, to) and the graph version they were found
/// on. Ask before searching; a stale or missing entry means search and
/// store. Fixed capacity, oldest entries go first.
class RouteCache {
public:
    explicit RouteCache(usize capacity = 512) : capacity_(capacity) {}

    const GraphPath* find(u32 from, u32 to, u64 version);
    const GraphPath& store(u32 from, u32 to, u64 version, GraphPath path);
    /// find(), else search with `compute` and store.
    template <class F> const GraphPath& get(u32 from, u32 to, u64 version, F&& compute) {
        if (const GraphPath* hit = find(from, to, version)) return *hit;
        return store(from, to, version, compute());
    }
    void clear();
    usize size() const { return entries_.size(); }
    u32 hits() const { return hits_; }
    u32 misses() const { return misses_; }

private:
    struct Entry {
        u64 version;
        GraphPath path;
    };
    static u64 key(u32 from, u32 to) { return (static_cast<u64>(from) << 32) | to; }
    usize capacity_;
    std::unordered_map<u64, Entry> entries_;
    std::deque<u64> order_;
    u32 hits_ = 0;
    u32 misses_ = 0;
};

} // namespace rw
