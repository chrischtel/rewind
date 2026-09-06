#include "rewind/sim/graph.h"

#include <algorithm>
#include <cmath>

namespace rw {

namespace {
constexpr float kInf = 1e30f;

/// Binary heap entry ordered by cost, then node id, so ties are deterministic.
struct Item {
    float cost;
    u32 node;
    bool operator>(const Item& o) const { return cost > o.cost || (cost == o.cost && node > o.node); }
};

void heap_push(std::vector<Item>& h, Item it) {
    h.push_back(it);
    std::push_heap(h.begin(), h.end(), std::greater<Item>{});
}
Item heap_pop(std::vector<Item>& h) {
    std::pop_heap(h.begin(), h.end(), std::greater<Item>{});
    Item it = h.back();
    h.pop_back();
    return it;
}
} // namespace

u32 Graph::add_node(Vec2 pos, u32 tag) {
    nodes_.push_back(GraphNode{pos, true, tag});
    out_.emplace_back();
    in_.emplace_back();
    ++version_;
    return static_cast<u32>(nodes_.size() - 1);
}

void Graph::remove_node(u32 node) {
    if (node >= nodes_.size()) return;
    nodes_[node].alive = false;
    for (GraphEdge& e : edges_)
        if (e.from == node || e.to == node) e.enabled = false;
    ++version_;
}

u32 Graph::add_edge(u32 from, u32 to, float cost, bool directed) {
    if (from >= nodes_.size() || to >= nodes_.size()) return kNone;
    const u32 id = static_cast<u32>(edges_.size());
    edges_.push_back(GraphEdge{from, to, cost, true});
    penalties_.push_back(0);
    out_[from].push_back(id);
    in_[to].push_back(id);
    if (!directed) {
        edges_.push_back(GraphEdge{to, from, cost, true});
        penalties_.push_back(0);
        out_[to].push_back(id + 1);
        in_[from].push_back(id + 1);
    }
    ++version_;
    return id;
}

void Graph::remove_edge(u32 edge) {
    if (edge < edges_.size()) {
        edges_[edge].enabled = false;
        ++version_;
    }
}

void Graph::set_edge_cost(u32 edge, float cost) {
    if (edge < edges_.size()) {
        edges_[edge].cost = cost;
        ++version_;
    }
}

void Graph::set_edge_penalty(u32 edge, float penalty) {
    if (edge >= edges_.size()) return;
    if (penalties_.size() < edges_.size()) penalties_.resize(edges_.size(), 0.0f);
    if (penalties_[edge] == penalty) return;
    penalties_[edge] = penalty;
    ++version_;
}

void Graph::clear_penalties() {
    penalties_.assign(edges_.size(), 0.0f);
    ++version_;
}

u32 Graph::connect(u32 from, u32 to, bool directed) {
    if (from >= nodes_.size() || to >= nodes_.size()) return kNone;
    return add_edge(from, to, distance(nodes_[from].pos, nodes_[to].pos), directed);
}

void Graph::clear() {
    nodes_.clear();
    edges_.clear();
    out_.clear();
    in_.clear();
    penalties_.clear();
    ++version_;
}

Graph Graph::grid(u32 w, u32 h, float cell, bool diagonals, const std::function<bool(u32, u32)>& passable, Vec2 origin) {
    Graph g;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const u32 id = g.add_node({origin.x + static_cast<float>(x) * cell, origin.y + static_cast<float>(y) * cell});
            if (passable && !passable(x, y)) g.nodes_[id].alive = false;
        }
    auto ok = [&](u32 x, u32 y) { return x < w && y < h && g.nodes_[y * w + x].alive; };
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            if (!ok(x, y)) continue;
            const u32 id = y * w + x;
            if (ok(x + 1, y)) g.add_edge(id, id + 1, cell);
            if (ok(x, y + 1)) g.add_edge(id, id + w, cell);
            if (diagonals) {
                const float d = cell * 1.41421356f;
                // No corner cutting: both orthogonal neighbours must be free.
                if (ok(x + 1, y + 1) && ok(x + 1, y) && ok(x, y + 1)) g.add_edge(id, id + w + 1, d);
                if (x > 0 && ok(x - 1, y + 1) && ok(x - 1, y) && ok(x, y + 1)) g.add_edge(id, id + w - 1, d);
            }
        }
    return g;
}

const std::vector<u32>& Graph::edges_from(u32 node) const {
    static const std::vector<u32> none;
    return node < out_.size() ? out_[node] : none;
}

const std::vector<u32>& Graph::edges_into(u32 node) const {
    static const std::vector<u32> none;
    return node < in_.size() ? in_[node] : none;
}

u32 Graph::find_edge(u32 from, u32 to) const {
    for (u32 e : edges_from(from))
        if (edges_[e].enabled && edges_[e].to == to) return e;
    return kNone;
}

u32 Graph::nearest_node(Vec2 p, float max_distance) const {
    u32 best = kNone;
    float best_d = max_distance * max_distance;
    for (u32 i = 0; i < nodes_.size(); ++i) {
        if (!nodes_[i].alive) continue;
        const float d = length_sq(nodes_[i].pos - p);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

GraphPath Graph::shortest_path(u32 from, u32 to, float heuristic_weight,
                               const std::function<bool(u32)>& passable) const {
    GraphPath result;
    if (!valid(from) || !valid(to)) return result;
    if (passable && (!passable(from) || !passable(to))) return result;
    const usize n = nodes_.size();
    std::vector<float> g(n, kInf);
    std::vector<u32> came(n, kNone);
    std::vector<bool> closed(n, false);
    const Vec2 goal = nodes_[to].pos;
    auto h = [&](u32 node) { return heuristic_weight > 0 ? distance(nodes_[node].pos, goal) * heuristic_weight : 0.0f; };

    std::vector<Item> open;
    g[from] = 0;
    heap_push(open, {h(from), from});
    while (!open.empty()) {
        const Item it = heap_pop(open);
        const u32 u = it.node;
        if (closed[u]) continue;
        closed[u] = true;
        if (u == to) break;
        for (u32 ei : out_[u]) {
            const GraphEdge& e = edges_[ei];
            if (!e.enabled || !nodes_[e.to].alive || closed[e.to]) continue;
            if (passable && !passable(e.to)) continue;
            const float cost = g[u] + std::max(edge_cost(ei), 0.0f);
            if (cost < g[e.to]) {
                g[e.to] = cost;
                came[e.to] = u;
                heap_push(open, {cost + h(e.to), e.to});
            }
        }
    }
    if (g[to] >= kInf) return result;
    result.found = true;
    result.cost = g[to];
    for (u32 cur = to; cur != kNone; cur = came[cur]) result.nodes.push_back(cur);
    std::reverse(result.nodes.begin(), result.nodes.end());
    return result;
}

std::vector<float> Graph::distances_from(u32 from, float max_cost) const {
    std::vector<float> dist(nodes_.size(), kInf);
    if (!valid(from)) return dist;
    std::vector<bool> closed(nodes_.size(), false);
    std::vector<Item> open;
    dist[from] = 0;
    heap_push(open, {0, from});
    while (!open.empty()) {
        const Item it = heap_pop(open);
        if (closed[it.node]) continue;
        closed[it.node] = true;
        for (u32 ei : out_[it.node]) {
            const GraphEdge& e = edges_[ei];
            if (!e.enabled || !nodes_[e.to].alive) continue;
            const float cost = dist[it.node] + std::max(edge_cost(ei), 0.0f);
            if (cost <= max_cost && cost < dist[e.to]) {
                dist[e.to] = cost;
                heap_push(open, {cost, e.to});
            }
        }
    }
    return dist;
}

std::vector<u32> Graph::reachable(u32 from, float max_cost) const {
    const std::vector<float> dist = distances_from(from, max_cost);
    std::vector<u32> out;
    for (u32 i = 0; i < dist.size(); ++i)
        if (dist[i] < kInf) out.push_back(i);
    std::sort(out.begin(), out.end(), [&](u32 a, u32 b) { return dist[a] < dist[b] || (dist[a] == dist[b] && a < b); });
    return out;
}

std::vector<Vec2> Graph::positions(const GraphPath& path) const {
    std::vector<Vec2> out;
    out.reserve(path.nodes.size());
    for (u32 n : path.nodes)
        if (n < nodes_.size()) out.push_back(nodes_[n].pos);
    return out;
}

void Graph::rebuild_adjacency() {
    out_.assign(nodes_.size(), {});
    in_.assign(nodes_.size(), {});
    for (u32 i = 0; i < edges_.size(); ++i) {
        if (edges_[i].from < nodes_.size()) out_[edges_[i].from].push_back(i);
        if (edges_[i].to < nodes_.size()) in_[edges_[i].to].push_back(i);
    }
    penalties_.assign(edges_.size(), 0.0f);
    ++version_;
}

GraphPath Graph::bidirectional_path(u32 from, u32 to, const std::function<bool(u32)>& passable) const {
    GraphPath result;
    if (!valid(from) || !valid(to)) return result;
    if (passable && (!passable(from) || !passable(to))) return result;
    if (from == to) {
        result.found = true;
        result.nodes.push_back(from);
        return result;
    }
    const usize n = nodes_.size();
    std::vector<float> gf(n, kInf), gb(n, kInf);
    std::vector<u32> came_f(n, kNone), came_b(n, kNone); // came_b: next node towards the goal
    std::vector<bool> closed_f(n, false), closed_b(n, false);
    std::vector<Item> open_f, open_b;
    gf[from] = 0;
    gb[to] = 0;
    heap_push(open_f, {0, from});
    heap_push(open_b, {0, to});
    float best = kInf;
    u32 meet = kNone;
    auto consider = [&](u32 node) {
        if (gf[node] < kInf && gb[node] < kInf) {
            const float total = gf[node] + gb[node];
            if (total < best || (total == best && node < meet)) {
                best = total;
                meet = node;
            }
        }
    };
    while (!open_f.empty() && !open_b.empty()) {
        // Stop once neither frontier can improve on the best meeting point.
        if (open_f.front().cost + open_b.front().cost >= best) break;
        // Alternate, smaller frontier first for balance.
        if (open_f.size() <= open_b.size()) {
            const Item it = heap_pop(open_f);
            const u32 u = it.node;
            if (closed_f[u]) continue;
            closed_f[u] = true;
            for (u32 ei : out_[u]) {
                const GraphEdge& e = edges_[ei];
                if (!e.enabled || !nodes_[e.to].alive || closed_f[e.to]) continue;
                if (passable && !passable(e.to)) continue;
                const float cost = gf[u] + std::max(edge_cost(ei), 0.0f);
                if (cost < gf[e.to]) {
                    gf[e.to] = cost;
                    came_f[e.to] = u;
                    heap_push(open_f, {cost, e.to});
                    consider(e.to);
                }
            }
        } else {
            const Item it = heap_pop(open_b);
            const u32 u = it.node;
            if (closed_b[u]) continue;
            closed_b[u] = true;
            for (u32 ei : in_[u]) {
                const GraphEdge& e = edges_[ei];
                if (!e.enabled || !nodes_[e.from].alive || closed_b[e.from]) continue;
                if (passable && !passable(e.from)) continue;
                const float cost = gb[u] + std::max(edge_cost(ei), 0.0f);
                if (cost < gb[e.from]) {
                    gb[e.from] = cost;
                    came_b[e.from] = u;
                    heap_push(open_b, {cost, e.from});
                    consider(e.from);
                }
            }
        }
    }
    if (meet == kNone) return result;
    result.found = true;
    result.cost = best;
    for (u32 cur = meet; cur != kNone; cur = came_f[cur]) result.nodes.push_back(cur);
    std::reverse(result.nodes.begin(), result.nodes.end());
    for (u32 cur = came_b[meet]; cur != kNone; cur = came_b[cur]) result.nodes.push_back(cur);
    return result;
}

Graph::FlowField Graph::flow_to(u32 goal, float max_cost) const {
    FlowField f;
    f.goal = goal;
    f.version = version_;
    f.next_edge.assign(nodes_.size(), kNone);
    f.distance.assign(nodes_.size(), kInf);
    if (!valid(goal)) return f;
    std::vector<bool> closed(nodes_.size(), false);
    std::vector<Item> open;
    f.distance[goal] = 0;
    heap_push(open, {0, goal});
    while (!open.empty()) {
        const Item it = heap_pop(open);
        if (closed[it.node]) continue;
        closed[it.node] = true;
        for (u32 ei : in_[it.node]) {
            const GraphEdge& e = edges_[ei];
            if (!e.enabled || !nodes_[e.from].alive) continue;
            const float cost = f.distance[it.node] + std::max(edge_cost(ei), 0.0f);
            if (cost <= max_cost && cost < f.distance[e.from]) {
                f.distance[e.from] = cost;
                f.next_edge[e.from] = ei;
                heap_push(open, {cost, e.from});
            }
        }
    }
    return f;
}

// ---- route cache -------------------------------------------------------------------------

const GraphPath* RouteCache::find(u32 from, u32 to, u64 version) {
    auto it = entries_.find(key(from, to));
    if (it == entries_.end() || it->second.version != version) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    return &it->second.path;
}

const GraphPath& RouteCache::store(u32 from, u32 to, u64 version, GraphPath path) {
    const u64 k = key(from, to);
    if (!entries_.contains(k)) {
        order_.push_back(k);
        while (order_.size() > capacity_) {
            entries_.erase(order_.front());
            order_.pop_front();
        }
    }
    Entry& e = entries_[k];
    e.version = version;
    e.path = std::move(path);
    return e.path;
}

void RouteCache::clear() {
    entries_.clear();
    order_.clear();
}

} // namespace rw
