#pragma once

// A weighted graph with shortest-path search. Nodes may carry a position
// (used as the A* heuristic and by nearest_node); edges carry a cost and
// can be one-way or disabled. Deterministic: ties break by node id, so a
// search gives the same route on every machine. Reflects, so a graph can be
// part of saved state or a scene.

#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/sim/reflect.h"

#include <functional>
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
    void remove_edge(u32 edge) { if (edge < edges_.size()) edges_[edge].enabled = false; }
    void set_edge_cost(u32 edge, float cost) { if (edge < edges_.size()) edges_[edge].cost = cost; }
    void clear();

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
    /// Enabled edge from a to b, or kNone.
    u32 find_edge(u32 from, u32 to) const;
    u32 nearest_node(Vec2 p, float max_distance = 1e30f) const;

    // ---- search ------------------------------------------------------------
    /// A* when nodes have positions (Euclidean heuristic scaled by `heuristic_weight`),
    /// plain Dijkstra with weight 0. Optional `passable(node)` filter.
    GraphPath shortest_path(u32 from, u32 to, float heuristic_weight = 1,
                            const std::function<bool(u32)>& passable = nullptr) const;
    /// Dijkstra flood: cost to reach every node from `from` (1e30 = unreachable), stopping at `max_cost`.
    std::vector<float> distances_from(u32 from, float max_cost = 1e30f) const;
    /// Nodes reachable within `max_cost`, nearest first.
    std::vector<u32> reachable(u32 from, float max_cost) const;
    /// Every path node's position, for drawing or following.
    std::vector<Vec2> positions(const GraphPath& path) const;

    void reflect(Visitor& v) {
        v.field("nodes", nodes_);
        v.field("edges", edges_);
        rebuild_adjacency();
    }

private:
    void rebuild_adjacency();

    std::vector<GraphNode> nodes_;
    std::vector<GraphEdge> edges_;
    std::vector<std::vector<u32>> out_; ///< per node: edge indices; derived
};

} // namespace rw
