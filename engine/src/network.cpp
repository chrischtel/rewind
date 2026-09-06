#include "rewind/sim/network.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rw {

namespace {
constexpr u32 kNone = ~0u;
constexpr float kFlattenTolerance = 0.5f;
} // namespace

// ---- authoring ------------------------------------------------------------------------

u32 CurveNetwork::add_node(Vec2 pos, float elevation, u32 tag, float radius) {
    nodes_.push_back(NetNode{pos, elevation, radius, tag, true});
    return static_cast<u32>(nodes_.size() - 1);
}

u32 CurveNetwork::add_edge(u32 a, u32 b, const Path* path) {
    if (!valid_node(a) || !valid_node(b) || a == b) return kNone;
    NetEdge e;
    e.a = a;
    e.b = b;
    if (path && !path->empty()) e.path = *path;
    else e.path = Path::from_polyline({nodes_[a].pos, nodes_[b].pos});
    snap_edge(e);
    edges_.push_back(std::move(e));
    return static_cast<u32>(edges_.size() - 1);
}

u32 CurveNetwork::add_curved_edge(u32 a, u32 b, Vec2 dir_a, Vec2 dir_b) {
    if (!valid_node(a) || !valid_node(b) || a == b) return kNone;
    const Vec2 pa = nodes_[a].pos, pb = nodes_[b].pos;
    const float reach = distance(pa, pb) / 3;
    Path p;
    p.segments.push_back({pa, pa + normalize(dir_a) * reach, pb - normalize(dir_b) * reach, pb});
    return add_edge(a, b, &p);
}

void CurveNetwork::remove_edge(u32 edge) {
    if (edge < edges_.size()) edges_[edge].alive = false;
}

void CurveNetwork::remove_node(u32 node) {
    if (node >= nodes_.size()) return;
    nodes_[node].alive = false;
    for (NetEdge& e : edges_)
        if (e.a == node || e.b == node) e.alive = false;
}

void CurveNetwork::set_tracks(u32 edge, u32 forward_count, u32 backward_count, float track_width) {
    if (!valid_edge(edge)) return;
    NetEdge& e = edges_[edge];
    e.tracks.clear();
    const u32 total = forward_count + backward_count;
    e.width = static_cast<float>(total) * track_width;
    // Backward tracks on the left (negative offsets), forward on the right,
    // like traffic that keeps right; a game wanting the mirror swaps the flag.
    const float first = -e.width * 0.5f + track_width * 0.5f;
    u32 i = 0;
    for (u32 k = 0; k < backward_count; ++k, ++i) e.tracks.push_back(Track{first + static_cast<float>(i) * track_width, false, track_width, 0});
    for (u32 k = 0; k < forward_count; ++k, ++i) e.tracks.push_back(Track{first + static_cast<float>(i) * track_width, true, track_width, 0});
}

u32 CurveNetwork::split_edge(u32 edge, usize segment, float t) {
    if (!valid_edge(edge)) return kNone;
    NetEdge& e = edges_[edge];
    if (segment >= e.path.segments.size()) return kNone;
    Path first, second;
    e.path.split(segment, t, first, second);
    const Vec2 at = first.end();
    const float ta = static_cast<float>(segment + t) / static_cast<float>(e.path.segments.size());
    const u32 mid = add_node(at, elevation_at(edge, ta), nodes_[e.a].tag);
    NetEdge tail = e;
    tail.a = mid;
    tail.path = second;
    e.b = mid;
    e.path = first;
    edges_.push_back(std::move(tail)); // may reallocate: nothing above is used after this
    return mid;
}

void CurveNetwork::move_node(u32 node, Vec2 pos) {
    if (valid_node(node)) nodes_[node].pos = pos;
}

void CurveNetwork::clear() {
    nodes_.clear();
    edges_.clear();
    rebuild();
}

void CurveNetwork::snap_edge(NetEdge& e) {
    if (e.path.empty()) e.path = Path::from_polyline({nodes_[e.a].pos, nodes_[e.b].pos});
    // Ends follow the nodes; handles keep their direction relative to the end.
    CubicBezier& f = e.path.segments.front();
    const Vec2 da = f.p1 - f.p0;
    f.p0 = nodes_[e.a].pos;
    f.p1 = f.p0 + da;
    CubicBezier& l = e.path.segments.back();
    const Vec2 db = l.p2 - l.p3;
    l.p3 = nodes_[e.b].pos;
    l.p2 = l.p3 + db;
    e.path.closed = false;
}

// ---- derived data -----------------------------------------------------------------------

void CurveNetwork::rebuild() {
    for (NetEdge& e : edges_)
        if (e.alive && valid_node(e.a) && valid_node(e.b)) snap_edge(e);
        else e.alive = false;
    build_tracks();
    build_connectors();
    build_links();
    build_route_graph();
    build_conflicts();
}

void CurveNetwork::build_tracks() {
    track_refs_.clear();
    track_first_.assign(edges_.size(), kNone);
    track_paths_.clear();
    track_lengths_.clear();
    for (u32 ei = 0; ei < edges_.size(); ++ei) {
        const NetEdge& e = edges_[ei];
        if (!e.alive) continue;
        track_first_[ei] = static_cast<u32>(track_refs_.size());
        for (u32 ti = 0; ti < e.tracks.size(); ++ti) {
            const Track& t = e.tracks[ti];
            Path p;
            if (absf(t.offset) < 1e-4f) p = e.path;
            else p = Path::from_polyline(e.path.offset(t.offset, kFlattenTolerance));
            if (!t.forward) p = p.reversed();
            // Stop short of junction nodes so connectors have room.
            const float ra = nodes_[t.forward ? e.a : e.b].radius, rb = nodes_[t.forward ? e.b : e.a].radius;
            if (ra > 0 || rb > 0) {
                const float len = p.length();
                if (len > ra + rb + 1.0f) p = p.sub_path(ra, len - rb, kFlattenTolerance);
            }
            track_refs_.push_back(TrackRef{ei, ti});
            track_lengths_.push_back(p.length());
            track_paths_.push_back(std::move(p));
        }
    }
}

Path CurveNetwork::connector_path(TrackRef from, TrackRef to) const {
    const Path& a = track_path(from);
    const Path& b = track_path(to);
    const Vec2 p0 = a.end(), p3 = b.start();
    const Vec2 d0 = a.segments.empty() ? Vec2{1, 0} : a.segments.back().direction(1);
    const Vec2 d3 = b.segments.empty() ? Vec2{1, 0} : b.segments.front().direction(0);
    const float reach = std::max(distance(p0, p3) * 0.4f, 0.01f);
    Path p;
    p.segments.push_back({p0, p0 + d0 * reach, p3 - d3 * reach, p3});
    return p;
}

void CurveNetwork::build_connectors() {
    connectors_.clear();
    // Which tracks arrive at and leave from each node.
    std::vector<std::vector<TrackRef>> arriving(nodes_.size()), leaving(nodes_.size());
    for (u32 i = 0; i < track_refs_.size(); ++i) {
        const TrackRef r = track_refs_[i];
        const NetEdge& e = edges_[r.edge];
        const bool fwd = e.tracks[r.track].forward;
        arriving[fwd ? e.b : e.a].push_back(r);
        leaving[fwd ? e.a : e.b].push_back(r);
    }
    for (u32 n = 0; n < nodes_.size(); ++n) {
        if (!nodes_[n].alive) continue;
        for (const TrackRef& from : arriving[n])
            for (const TrackRef& to : leaving[n]) {
                const bool allowed = filter_ ? filter_(*this, n, from, to) : from.edge != to.edge;
                if (!allowed) continue;
                Connector c;
                c.node = n;
                c.from = from;
                c.to = to;
                c.path = connector_path(from, to);
                c.length = c.path.length();
                connectors_.push_back(std::move(c));
            }
    }
}

void CurveNetwork::build_links() {
    links_.clear();
    for (u32 ei = 0; ei < edges_.size(); ++ei) {
        const NetEdge& e = edges_[ei];
        if (!e.alive) continue;
        // Neighbours: same direction, nearest offsets on either side.
        for (u32 i = 0; i < e.tracks.size(); ++i)
            for (u32 j = 0; j < e.tracks.size(); ++j) {
                if (i == j || e.tracks[i].forward != e.tracks[j].forward) continue;
                const float gap = e.tracks[j].offset - e.tracks[i].offset;
                bool nearest = true;
                for (u32 k = 0; k < e.tracks.size(); ++k) {
                    if (k == i || k == j || e.tracks[k].forward != e.tracks[i].forward) continue;
                    const float g = e.tracks[k].offset - e.tracks[i].offset;
                    if ((g > 0) == (gap > 0) && absf(g) < absf(gap)) nearest = false;
                }
                if (nearest) links_.push_back(TrackLink{TrackRef{ei, i}, TrackRef{ei, j}});
            }
    }
}

void CurveNetwork::build_route_graph() {
    route_graph_.clear();
    route_edge_kind_.clear();
    for (u32 i = 0; i < track_refs_.size(); ++i) route_graph_.add_node(track_paths_[i].start(), i);
    for (u32 c = 0; c < connectors_.size(); ++c) {
        const Connector& k = connectors_[c];
        const u32 to = track_index(k.to);
        route_graph_.add_edge(track_index(k.from), to, k.length + track_lengths_[to], true);
        route_edge_kind_.push_back(static_cast<i32>(c));
    }
    for (u32 l = 0; l < links_.size(); ++l) {
        const TrackLink& k = links_[l];
        const u32 to = track_index(k.to);
        route_graph_.add_edge(track_index(k.from), to, change_penalty + track_lengths_[to] * 0.5f, true);
        route_edge_kind_.push_back(-static_cast<i32>(l) - 1);
    }
}

void CurveNetwork::build_conflicts() {
    conflicts_.clear();
    std::vector<std::vector<Vec2>> flat(connectors_.size());
    for (u32 c = 0; c < connectors_.size(); ++c) connectors_[c].path.flatten(kFlattenTolerance, flat[c]);
    for (u32 a = 0; a < connectors_.size(); ++a)
        for (u32 b = a + 1; b < connectors_.size(); ++b) {
            if (connectors_[a].node != connectors_[b].node) continue;
            // Sharing a start or end is merging or diverging, not crossing.
            if (connectors_[a].from == connectors_[b].from || connectors_[a].to == connectors_[b].to) continue;
            bool found = false;
            Vec2 where;
            for (usize i = 0; i + 1 < flat[a].size() && !found; ++i)
                for (usize j = 0; j + 1 < flat[b].size(); ++j)
                    if (segments_intersect(flat[a][i], flat[a][i + 1], flat[b][j], flat[b][j + 1], &where)) {
                        found = true;
                        break;
                    }
            if (found) conflicts_.push_back(Conflict{connectors_[a].node, a, b, where});
        }
}

// ---- access -------------------------------------------------------------------------------

std::vector<u32> CurveNetwork::edges_at(u32 node) const {
    std::vector<u32> out;
    for (u32 i = 0; i < edges_.size(); ++i)
        if (edges_[i].alive && (edges_[i].a == node || edges_[i].b == node)) out.push_back(i);
    return out;
}

std::vector<u32> CurveNetwork::connectors_at(u32 node) const {
    std::vector<u32> out;
    for (u32 i = 0; i < connectors_.size(); ++i)
        if (connectors_[i].node == node) out.push_back(i);
    return out;
}

std::vector<u32> CurveNetwork::connectors_from(TrackRef track) const {
    std::vector<u32> out;
    for (u32 i = 0; i < connectors_.size(); ++i)
        if (connectors_[i].from == track) out.push_back(i);
    return out;
}

u32 CurveNetwork::track_index(TrackRef t) const {
    if (t.edge >= track_first_.size() || track_first_[t.edge] == kNone) return kNone;
    const u32 i = track_first_[t.edge] + t.track;
    return i < track_refs_.size() && track_refs_[i] == t ? i : kNone;
}

const Path& CurveNetwork::track_path(TrackRef t) const {
    static const Path empty;
    const u32 i = track_index(t);
    return i == kNone ? empty : track_paths_[i];
}

float CurveNetwork::track_length(TrackRef t) const {
    const u32 i = track_index(t);
    return i == kNone ? 0.0f : track_lengths_[i];
}

float CurveNetwork::elevation_at(u32 edge, float t) const {
    if (!valid_edge(edge)) return 0;
    const NetEdge& e = edges_[edge];
    return lerpf(nodes_[e.a].elevation, nodes_[e.b].elevation, clampf(t, 0, 1));
}

PathPoint CurveNetwork::point_at(const Location& loc) const {
    PathSampler s(track_path(loc.track), kFlattenTolerance);
    return s.at(loc.distance);
}

u32 CurveNetwork::nearest_node(Vec2 p, float max_distance) const {
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

u32 CurveNetwork::nearest_edge(Vec2 p, float max_distance, float* t_out) const {
    u32 best = kNone;
    float best_d = max_distance;
    float best_t = 0;
    for (u32 i = 0; i < edges_.size(); ++i) {
        if (!edges_[i].alive) continue;
        PathSampler s(edges_[i].path, kFlattenTolerance);
        const PathPoint c = s.closest(p);
        const float d = distance(c.position, p);
        if (d < best_d) {
            best_d = d;
            best = i;
            best_t = s.length() > 0 ? c.distance / s.length() : 0;
        }
    }
    if (t_out) *t_out = best_t;
    return best;
}

bool CurveNetwork::locate(Vec2 p, Location& out, float max_distance) const {
    float best_d = max_distance;
    bool found = false;
    for (u32 i = 0; i < track_refs_.size(); ++i) {
        PathSampler s(track_paths_[i], kFlattenTolerance);
        const PathPoint c = s.closest(p);
        const float d = distance(c.position, p);
        if (d < best_d) {
            best_d = d;
            out.track = track_refs_[i];
            out.distance = c.distance;
            found = true;
        }
    }
    return found;
}

// ---- routing ------------------------------------------------------------------------------

Route CurveNetwork::route(TrackRef from, TrackRef to) const {
    Route r;
    const u32 a = track_index(from), b = track_index(to);
    if (a == kNone || b == kNone) return r;
    r.steps.push_back(RouteStep{RouteStep::Kind::Track, a});
    if (a == b) {
        r.found = true;
        return r;
    }
    const GraphPath gp = route_graph_.shortest_path(a, b);
    if (!gp.found) return r;
    r.found = true;
    r.cost = gp.cost;
    for (usize i = 0; i + 1 < gp.nodes.size(); ++i) {
        const u32 ge = route_graph_.find_edge(gp.nodes[i], gp.nodes[i + 1]);
        if (ge == Graph::kNone) {
            r.found = false;
            r.steps.clear();
            return r;
        }
        const i32 kind = route_edge_kind_[ge];
        if (kind >= 0) r.steps.push_back(RouteStep{RouteStep::Kind::Connector, static_cast<u32>(kind)});
        else r.steps.push_back(RouteStep{RouteStep::Kind::Change, static_cast<u32>(-kind - 1)});
        r.steps.push_back(RouteStep{RouteStep::Kind::Track, gp.nodes[i + 1]});
    }
    return r;
}

Route CurveNetwork::route(const Location& from, const Location& to) const {
    Route r = route(from.track, to.track);
    if (r.found && from.track == to.track && to.distance < from.distance) {
        // Behind us on the same track: go around through a connector back onto it.
        const u32 a = track_index(from.track);
        Route best;
        for (u32 c : connectors_from(from.track)) {
            const u32 next = track_index(connectors_[c].to);
            Route back = route(connectors_[c].to, from.track);
            if (!back.found) continue;
            const float cost = connectors_[c].length + track_lengths_[next] + back.cost;
            if (!best.found || cost < best.cost) {
                best.found = true;
                best.cost = cost;
                best.steps = {RouteStep{RouteStep::Kind::Track, a}, RouteStep{RouteStep::Kind::Connector, c}};
                best.steps.insert(best.steps.end(), back.steps.begin(), back.steps.end());
            }
        }
        return best;
    }
    return r;
}

Path CurveNetwork::route_path(const Route& r, const Location* from, const Location* to) const {
    Path out;
    if (!r.found) return out;
    for (usize i = 0; i < r.steps.size(); ++i) {
        const RouteStep& s = r.steps[i];
        Path piece;
        if (s.kind == RouteStep::Kind::Track) {
            piece = track_paths_[s.index];
            const bool first = i == 0, last = i + 1 == r.steps.size();
            float d0 = 0, d1 = track_lengths_[s.index];
            if (first && from) d0 = clampf(from->distance, 0, d1);
            if (last && to) d1 = clampf(to->distance, 0, d1);
            if (d0 > 0 || d1 < track_lengths_[s.index]) piece = piece.sub_path(d0, d1, kFlattenTolerance);
        } else if (s.kind == RouteStep::Kind::Connector) {
            piece = connectors_[s.index].path;
        } else {
            // A sideways change: a short cubic from the current spot on one
            // track to the matching spot on the neighbour, folded into the
            // next track piece by joining (the join bridges the gap).
            continue;
        }
        if (piece.empty()) continue;
        out.join(piece);
    }
    return out;
}

} // namespace rw
