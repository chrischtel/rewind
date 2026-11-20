#include "rewind/sim/network.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rw {

namespace {
constexpr u32 kNone = ~0u;
constexpr float kFlattenTolerance = 0.5f;
const std::vector<u32> kNoIndices;

/// FNV-1a over the fields that derived data depends on. Fields are fed one
/// by one, never whole structs, so padding never gets a say.
struct Hasher {
    u64 h = 1469598103934665603ull;
    void bytes(const void* data, usize size) {
        const u8* p = static_cast<const u8*>(data);
        for (usize i = 0; i < size; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    }
    void add(float v) { bytes(&v, sizeof v); }
    void add(u32 v) { bytes(&v, sizeof v); }
    void add(i32 v) { bytes(&v, sizeof v); }
    void add(bool v) {
        const u8 b = v ? 1 : 0;
        bytes(&b, 1);
    }
    void add(Vec2 v) {
        add(v.x);
        add(v.y);
    }
    void add(TrackRef t) {
        add(t.edge);
        add(t.track);
    }
};

u64 signature(const NetNode& n) {
    Hasher h;
    h.add(n.alive);
    h.add(n.pos);
    h.add(n.radius);
    h.add(n.tag);
    h.add(n.authored);
    h.add(static_cast<u32>(n.turns.size()));
    for (const Turn& t : n.turns) {
        h.add(t.from);
        h.add(t.to);
        h.add(t.group);
        h.add(t.tag);
        h.add(t.priority);
    }
    return h.h;
}

u64 signature(const NetEdge& e) {
    Hasher h;
    h.add(e.alive);
    h.add(e.a);
    h.add(e.b);
    h.add(e.tag);
    h.add(e.width);
    h.add(static_cast<u32>(e.tracks.size()));
    for (const Track& t : e.tracks) {
        h.add(t.offset);
        h.add(t.forward);
        h.add(t.width);
        h.add(t.tag);
        h.add(t.begin);
        h.add(t.end);
        h.add(t.taper);
    }
    h.add(static_cast<u32>(e.path.segments.size()));
    for (const CubicBezier& s : e.path.segments) {
        h.add(s.p0);
        h.add(s.p1);
        h.add(s.p2);
        h.add(s.p3);
    }
    return h.h;
}

/// Distance from a point to a rectangle, 0 inside it.
float rect_distance(const Rect& r, Vec2 p) {
    const float dx = std::max({r.x - p.x, 0.0f, p.x - (r.x + r.w)});
    const float dy = std::max({r.y - p.y, 0.0f, p.y - (r.y + r.h)});
    return std::sqrt(dx * dx + dy * dy);
}
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
    adjacency_valid_ = false;
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
    adjacency_valid_ = false;
}

void CurveNetwork::remove_node(u32 node) {
    if (node >= nodes_.size()) return;
    nodes_[node].alive = false;
    for (NetEdge& e : edges_)
        if (e.a == node || e.b == node) e.alive = false;
    adjacency_valid_ = false;
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
    const float whole = e.path.length();
    Path first, second;
    e.path.split(segment, t, first, second);
    const Vec2 at = first.end();
    const float cut = first.length(); // how far along the edge the new node sits
    const float ta = static_cast<float>(segment + t) / static_cast<float>(e.path.segments.size());
    const u32 mid = add_node(at, elevation_at(edge, ta), nodes_[e.a].tag);

    // A track that runs only part of the edge goes to whichever piece it lies
    // on, or to both with its ends measured again; where the cut goes through
    // it, it now runs up to the new node. Each piece numbers its tracks
    // afresh, so remember who became whom.
    std::vector<Track> head_tracks, tail_tracks;
    std::vector<u32> to_head(e.tracks.size(), kNone), to_tail(e.tracks.size(), kNone);
    for (u32 i = 0; i < e.tracks.size(); ++i) {
        const Track& track = e.tracks[i];
        const float from = track.begin, to = whole - track.end; // where it runs, along the old edge
        if (from < cut - 1e-3f) {
            Track piece = track;
            piece.end = std::max(cut - to, 0.0f);
            to_head[i] = static_cast<u32>(head_tracks.size());
            head_tracks.push_back(piece);
        }
        if (to > cut + 1e-3f) {
            Track piece = track;
            piece.begin = std::max(from - cut, 0.0f);
            to_tail[i] = static_cast<u32>(tail_tracks.size());
            tail_tracks.push_back(piece);
        }
    }

    NetEdge tail = e;
    tail.a = mid;
    tail.path = second;
    tail.tracks = std::move(tail_tracks);
    e.b = mid;
    e.path = first;
    e.tracks = std::move(head_tracks);
    const u32 near_node = e.a, far_node = tail.b;
    const u32 tail_index = static_cast<u32>(edges_.size());
    edges_.push_back(std::move(tail)); // may reallocate: `e` is not used after this
    adjacency_valid_ = false;

    // Authored turns at the two old ends knew this road by its old index and
    // its old track numbers; they follow the piece that still reaches them.
    const auto follow = [&](std::vector<Turn>& turns, const std::vector<u32>& map, u32 now) {
        for (usize i = 0; i < turns.size();) {
            bool gone = false;
            for (TrackRef* ref : {&turns[i].from, &turns[i].to}) {
                if (ref->edge != edge) continue;
                const u32 moved = ref->track < map.size() ? map[ref->track] : kNone;
                if (moved == kNone) gone = true;
                else *ref = TrackRef{now, moved};
            }
            if (gone) turns.erase(turns.begin() + static_cast<std::ptrdiff_t>(i));
            else ++i;
        }
    };
    follow(nodes_[far_node].turns, to_tail, tail_index);
    if (near_node != far_node) follow(nodes_[near_node].turns, to_head, edge);
    return mid;
}

// ---- authored junctions ---------------------------------------------------------------

bool CurveNetwork::arrives_at(TrackRef track, u32 node) const {
    if (!valid_edge(track.edge)) return false;
    const NetEdge& e = edges_[track.edge];
    if (track.track >= e.tracks.size()) return false;
    const Track& t = e.tracks[track.track];
    // A track that stops short of the node does not arrive there.
    return t.forward ? (e.b == node && t.end <= 0) : (e.a == node && t.begin <= 0);
}

bool CurveNetwork::leaves_from(TrackRef track, u32 node) const {
    if (!valid_edge(track.edge)) return false;
    const NetEdge& e = edges_[track.edge];
    if (track.track >= e.tracks.size()) return false;
    const Track& t = e.tracks[track.track];
    return t.forward ? (e.a == node && t.begin <= 0) : (e.b == node && t.end <= 0);
}

void CurveNetwork::author_node(u32 node) {
    if (!valid_node(node) || nodes_[node].authored) return;
    NetNode& n = nodes_[node];
    n.turns.clear();
    for (u32 c : connectors_at(node)) {
        const Connector& k = connectors_[c];
        n.turns.push_back(Turn{k.from, k.to, k.group, k.tag, k.priority});
    }
    n.authored = true;
}

void CurveNetwork::release_node(u32 node) {
    if (!valid_node(node)) return;
    nodes_[node].authored = false;
    nodes_[node].turns.clear();
}

Turn* CurveNetwork::find_turn(u32 node, TrackRef from, TrackRef to) {
    if (!valid_node(node)) return nullptr;
    for (Turn& t : nodes_[node].turns)
        if (t.from == from && t.to == to) return &t;
    return nullptr;
}

const Turn* CurveNetwork::find_turn(u32 node, TrackRef from, TrackRef to) const {
    if (!valid_node(node)) return nullptr;
    for (const Turn& t : nodes_[node].turns)
        if (t.from == from && t.to == to) return &t;
    return nullptr;
}

bool CurveNetwork::allow_turn(u32 node, TrackRef from, TrackRef to, bool allowed) {
    if (!valid_node(node) || !arrives_at(from, node) || !leaves_from(to, node)) return false;
    author_node(node);
    NetNode& n = nodes_[node];
    for (usize i = 0; i < n.turns.size(); ++i) {
        if (!(n.turns[i].from == from && n.turns[i].to == to)) continue;
        if (!allowed) n.turns.erase(n.turns.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    if (allowed) n.turns.push_back(Turn{from, to, 0, 0, 0});
    return true;
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
    adjacency_valid_ = false;

    const u32 edge_count = static_cast<u32>(edges_.size()), node_count = static_cast<u32>(nodes_.size());

    // ---- what changed since last time -----------------------------------------------
    std::vector<u64> node_sig(node_count), edge_sig(edge_count);
    for (u32 n = 0; n < node_count; ++n) node_sig[n] = signature(nodes_[n]);
    for (u32 e = 0; e < edge_count; ++e) edge_sig[e] = signature(edges_[e]);
    // Settings every track or junction depends on: a change makes everything fresh.
    Hasher settings;
    settings.add(merge_conflicts);
    settings.add(track_tolerance);
    const u64 config = settings.h;
    const bool everything = all_dirty_ || config != config_sig_;

    // A node is dirty when it changed itself or any edge at it did, now or
    // as it was before. An edge needs its tracks again when it changed or
    // either of its nodes is dirty (the trim at a node looks at every arm).
    // A node needs its connectors again when it is dirty or a track ending
    // there was rebuilt.
    std::vector<u8> node_dirty(node_count, 0), track_dirty(edge_count, 0), connector_dirty(node_count, 0);
    const auto touch = [&](u32 n) {
        if (n < node_count) node_dirty[n] = 1;
    };
    for (u32 n = 0; n < node_count; ++n)
        if (everything || n >= node_sig_.size() || node_sig_[n] != node_sig[n]) node_dirty[n] = 1;
    for (u32 e = 0; e < edge_count; ++e) {
        if (!everything && e < edge_sig_.size() && edge_sig_[e] == edge_sig[e]) continue;
        track_dirty[e] = 1;
        touch(edges_[e].a);
        touch(edges_[e].b);
        if (e < edge_a_.size()) { // where it used to be attached
            touch(edge_a_[e]);
            touch(edge_b_[e]);
        }
    }
    for (u32 e = 0; e < edge_count; ++e)
        if (edges_[e].alive && (node_dirty[edges_[e].a] || node_dirty[edges_[e].b])) track_dirty[e] = 1;
    for (u32 n = 0; n < node_count; ++n) connector_dirty[n] = node_dirty[n];
    for (u32 e = 0; e < edge_count; ++e)
        if (track_dirty[e]) {
            if (edges_[e].a < node_count) connector_dirty[edges_[e].a] = 1;
            if (edges_[e].b < node_count) connector_dirty[edges_[e].b] = 1;
        }

    stats_ = RebuildStats{};
    stats_.everything = everything;

    // ---- tracks: carried over where the edge is untouched ---------------------------
    {
        std::vector<TrackRef> refs;
        std::vector<u32> first(edge_count, kNone);
        std::vector<Path> paths;
        std::vector<float> lengths, alongs;
        refs.reserve(track_refs_.size());
        paths.reserve(track_paths_.size());
        lengths.reserve(track_lengths_.size());
        alongs.reserve(track_along_.size());
        for (u32 ei = 0; ei < edge_count; ++ei) {
            const NetEdge& e = edges_[ei];
            if (!e.alive) continue;
            ++stats_.edges;
            first[ei] = static_cast<u32>(refs.size());
            const u32 old = ei < track_first_.size() ? track_first_[ei] : kNone;
            const bool keep = !track_dirty[ei] && old != kNone && old + e.tracks.size() <= track_paths_.size();
            if (!keep) ++stats_.edges_rebuilt;
            for (u32 ti = 0; ti < e.tracks.size(); ++ti) {
                refs.push_back(TrackRef{ei, ti});
                if (keep) {
                    paths.push_back(std::move(track_paths_[old + ti]));
                    lengths.push_back(track_lengths_[old + ti]);
                    alongs.push_back(track_along_[old + ti]);
                } else {
                    Path path;
                    float length = 0, along = 0;
                    build_track(ei, ti, path, length, along);
                    paths.push_back(std::move(path));
                    lengths.push_back(length);
                    alongs.push_back(along);
                }
            }
        }
        track_refs_.swap(refs);
        track_first_.swap(first);
        track_paths_.swap(paths);
        track_lengths_.swap(lengths);
        track_along_.swap(alongs);
    }

    // ---- connectors: node by node, carried over where nothing moved -----------------
    std::vector<u32> moved_to(connectors_.size(), kNone); // old connector index -> new
    {
        std::vector<Connector> connectors;
        connectors.reserve(connectors_.size());
        for (u32 n = 0; n < node_count; ++n) {
            if (!nodes_[n].alive) continue;
            ++stats_.nodes;
            if (!connector_dirty[n] && n < node_connectors_.size()) {
                for (u32 c : node_connectors_[n]) {
                    moved_to[c] = static_cast<u32>(connectors.size());
                    connectors.push_back(std::move(connectors_[c]));
                }
            } else {
                ++stats_.nodes_rebuilt;
                derive_connectors(n, connectors);
            }
        }
        connectors_.swap(connectors);
    }
    build_lookups();
    build_links();
    build_route_graph();

    // ---- conflicts: the same way ---------------------------------------------------------
    {
        std::vector<Conflict> conflicts;
        conflicts.reserve(conflicts_.size());
        // Connectors are numbered node by node, so going node by node keeps
        // the list in connector order, which is the order it always had; and
        // the old list is in node order too, so one walk along it finds each
        // node's conflicts as that node comes up.
        usize old = 0;
        for (u32 n = 0; n < node_count; ++n) {
            while (old < conflicts_.size() && conflicts_[old].node < n) ++old;
            const usize begin = old;
            while (old < conflicts_.size() && conflicts_[old].node == n) ++old;
            if (!nodes_[n].alive) continue;
            if (connector_dirty[n]) {
                find_conflicts(node_connectors_[n], conflicts);
                continue;
            }
            for (usize i = begin; i < old; ++i) {
                Conflict moved = conflicts_[i];
                if (moved.connector_a >= moved_to.size() || moved.connector_b >= moved_to.size()) continue;
                moved.connector_a = moved_to[moved.connector_a];
                moved.connector_b = moved_to[moved.connector_b];
                if (moved.connector_a != kNone && moved.connector_b != kNone) conflicts.push_back(moved);
            }
        }
        conflicts_.swap(conflicts);
        connector_conflicts_.resize(connectors_.size());
        for (std::vector<u32>& l : connector_conflicts_) l.clear();
        for (u32 i = 0; i < conflicts_.size(); ++i) {
            connector_conflicts_[conflicts_[i].connector_a].push_back(i);
            connector_conflicts_[conflicts_[i].connector_b].push_back(i);
        }
    }

    // ---- remember what this was built from ------------------------------------------------
    node_sig_.swap(node_sig);
    edge_sig_.swap(edge_sig);
    edge_a_.resize(edge_count);
    edge_b_.resize(edge_count);
    for (u32 e = 0; e < edge_count; ++e) {
        edge_a_[e] = edges_[e].a;
        edge_b_[e] = edges_[e].b;
    }
    config_sig_ = config;
    all_dirty_ = false;

    ++version_;
    rebuilt_version_ = version_;
    track_changed_.assign(track_refs_.size(), version_);
    apply_penalties();
    cache_.clear();
}

void CurveNetwork::apply_penalties() {
    for (const auto& [track, penalty] : penalties_) {
        const u32 i = track_index(track);
        if (i == kNone) continue;
        for (u32 ge : route_graph_.edges_into(i)) route_graph_.set_edge_penalty(ge, penalty);
    }
}

void CurveNetwork::set_track_penalty(TrackRef track, float penalty) {
    bool found = false;
    for (auto& [t, p] : penalties_)
        if (t == track) {
            if (p == penalty) return;
            p = penalty;
            found = true;
        }
    if (!found) penalties_.emplace_back(track, penalty);
    const u32 i = track_index(track);
    ++version_;
    if (i == kNone) return;
    for (u32 ge : route_graph_.edges_into(i)) route_graph_.set_edge_penalty(ge, penalty);
    track_changed_[i] = version_;
}

float CurveNetwork::track_penalty(TrackRef track) const {
    for (const auto& [t, p] : penalties_)
        if (t == track) return p;
    return 0.0f;
}

void CurveNetwork::clear_track_penalties() {
    if (penalties_.empty()) return;
    penalties_.clear();
    route_graph_.clear_penalties();
    ++version_;
    track_changed_.assign(track_refs_.size(), version_);
}

bool CurveNetwork::route_affected(const Route& r) const {
    if (!r.found) return true;
    if (r.version < rebuilt_version_) return true;
    for (const RouteStep& s : r.steps)
        if (s.kind == RouteStep::Kind::Track && s.index < track_changed_.size() && track_changed_[s.index] > r.version) return true;
    return false;
}

void CurveNetwork::build_track(u32 ei, u32 ti, Path& p, float& length, float& along) const {
    const NetEdge& e = edges_[ei];
    const Track& t = e.tracks[ti];
    if (absf(t.offset) < 1e-4f) p = e.path;
    else p = Path::from_polyline(e.path.offset(t.offset, std::max(track_tolerance, 0.001f)));
    if (!t.forward) p = p.reversed();
    // In the direction of travel: stop short of junction nodes so connectors
    // have room, and short of wherever the track itself begins or ends.
    const u32 na = t.forward ? e.a : e.b, nb = t.forward ? e.b : e.a;
    const float ra = trim_ ? trim_(*this, na, ei) : nodes_[na].radius;
    const float rb = trim_ ? trim_(*this, nb, ei) : nodes_[nb].radius;
    const float from = std::max(ra, t.forward ? t.begin : t.end);
    const float short_by = std::max(rb, t.forward ? t.end : t.begin);
    along = 0;
    if (from > 0 || short_by > 0) {
        const float len = p.length();
        if (len > from + short_by + 1.0f) {
            p = p.sub_path(from, len - short_by, std::max(track_tolerance, 0.001f));
            along = from;
        } else if (!t.whole()) {
            // A partial track with no room left: keep a token unit of it
            // where it would have started, rather than the whole edge.
            const float at = clampf(from, 0, std::max(len - 1.0f, 0.0f));
            p = p.sub_path(at, std::min(at + 1.0f, len), std::max(track_tolerance, 0.001f));
            along = at;
        }
    }
    length = p.length();
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

void CurveNetwork::derive_connectors(u32 n, std::vector<Connector>& out) {
    const auto add = [&](const Turn* turn, TrackRef from, TrackRef to) {
        Connector c;
        c.node = n;
        c.from = from;
        c.to = to;
        if (turn) {
            c.group = turn->group;
            c.tag = turn->tag;
            c.priority = turn->priority;
        }
        c.path = connector_path(from, to);
        c.length = c.path.length();
        out.push_back(std::move(c));
    };
    if (nodes_[n].authored) {
        // Roads may have been removed or relaned since a turn was drawn:
        // what no longer runs through here is dropped for good.
        std::vector<Turn>& turns = nodes_[n].turns;
        turns.erase(std::remove_if(turns.begin(), turns.end(),
                                   [&](const Turn& t) { return !arrives_at(t.from, n) || !leaves_from(t.to, n); }),
                    turns.end());
        for (const Turn& t : turns) add(&t, t.from, t.to);
        return;
    }
    // Which tracks arrive at and leave from this node, in edge and track order.
    if (!adjacency_valid_) build_adjacency();
    std::vector<TrackRef> arriving, leaving;
    for (u32 ei : adjacency_[n])
        for (u32 ti = 0; ti < edges_[ei].tracks.size(); ++ti) {
            const TrackRef r{ei, ti};
            if (arrives_at(r, n)) arriving.push_back(r);
            if (leaves_from(r, n)) leaving.push_back(r);
        }
    for (const TrackRef& from : arriving)
        for (const TrackRef& to : leaving) {
            const bool allowed = filter_ ? filter_(*this, n, from, to) : from.edge != to.edge;
            if (allowed) add(nullptr, from, to);
        }
}

/// Lookup tables: by node, by the track a connector leaves, by the one it enters.
void CurveNetwork::build_lookups() {
    // Emptied rather than thrown away: thousands of short lists keep their memory.
    const auto empty = [](std::vector<std::vector<u32>>& lists, usize n) {
        lists.resize(n);
        for (std::vector<u32>& l : lists) l.clear();
    };
    empty(node_connectors_, nodes_.size());
    empty(track_connectors_, track_refs_.size());
    empty(track_feeders_, track_refs_.size());
    for (u32 c = 0; c < connectors_.size(); ++c) {
        const Connector& k = connectors_[c];
        node_connectors_[k.node].push_back(c);
        const u32 from = track_index(k.from), to = track_index(k.to);
        if (from != kNone) track_connectors_[from].push_back(c);
        if (to != kNone) track_feeders_[to].push_back(c);
    }
}

void CurveNetwork::build_links() {
    links_.clear();
    for (u32 ei = 0; ei < edges_.size(); ++ei) {
        const NetEdge& e = edges_[ei];
        if (!e.alive) continue;
        const u32 base = track_first_[ei];
        // Two tracks can only be changed between where both exist.
        const auto overlap = [&](u32 i, u32 j) {
            const float a0 = track_along_[base + i], a1 = a0 + track_lengths_[base + i];
            const float b0 = track_along_[base + j], b1 = b0 + track_lengths_[base + j];
            return std::min(a1, b1) - std::max(a0, b0);
        };
        // Neighbours: same direction, nearest offsets on either side.
        for (u32 i = 0; i < e.tracks.size(); ++i)
            for (u32 j = 0; j < e.tracks.size(); ++j) {
                if (i == j || e.tracks[i].forward != e.tracks[j].forward) continue;
                const float gap = e.tracks[j].offset - e.tracks[i].offset;
                bool nearest = true;
                for (u32 k = 0; k < e.tracks.size(); ++k) {
                    if (k == i || k == j || e.tracks[k].forward != e.tracks[i].forward) continue;
                    const float g = e.tracks[k].offset - e.tracks[i].offset;
                    if ((g > 0) == (gap > 0) && absf(g) < absf(gap) && overlap(i, k) > 1.0f) nearest = false;
                }
                if (nearest && overlap(i, j) > 1.0f) links_.push_back(TrackLink{TrackRef{ei, i}, TrackRef{ei, j}});
            }
    }
}

void CurveNetwork::build_route_graph() {
    route_graph_.reset();
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

/// The conflicts among the connectors of one node, appended in connector order.
void CurveNetwork::find_conflicts(const std::vector<u32>& here, std::vector<Conflict>& out) const {
    std::vector<std::vector<Vec2>> flat(here.size());
    for (usize i = 0; i < here.size(); ++i) connectors_[here[i]].path.flatten(kFlattenTolerance, flat[i]);
    // How far along a flattened curve its point `index` lies.
    const auto walked = [](const std::vector<Vec2>& line, usize index, Vec2 then) {
        float d = 0;
        for (usize i = 0; i < index; ++i) d += distance(line[i], line[i + 1]);
        return d + distance(line[index], then);
    };
    for (usize x = 0; x < here.size(); ++x)
        for (usize y = x + 1; y < here.size(); ++y) {
            const u32 a = here[x], b = here[y];
            if (connectors_[a].from == connectors_[b].from) continue; // diverging: one vehicle, one choice
            if (connectors_[a].to == connectors_[b].to) {
                // Merging, not crossing: they meet where the shared track starts.
                if (merge_conflicts)
                    out.push_back(Conflict{connectors_[a].node, a, b, connectors_[a].path.end(), ConflictKind::Merge,
                                           connectors_[a].length, connectors_[b].length});
                continue;
            }
            bool found = false;
            Vec2 where;
            usize ia = 0, ib = 0;
            for (usize i = 0; i + 1 < flat[x].size() && !found; ++i)
                for (usize j = 0; j + 1 < flat[y].size(); ++j)
                    if (segments_intersect(flat[x][i], flat[x][i + 1], flat[y][j], flat[y][j + 1], &where)) {
                        found = true;
                        ia = i;
                        ib = j;
                        break;
                    }
            if (found)
                out.push_back(Conflict{connectors_[a].node, a, b, where, ConflictKind::Crossing,
                                       walked(flat[x], ia, where), walked(flat[y], ib, where)});
        }
}

// ---- access -------------------------------------------------------------------------------

void CurveNetwork::build_adjacency() const {
    adjacency_.assign(nodes_.size(), {});
    for (u32 i = 0; i < edges_.size(); ++i) {
        const NetEdge& e = edges_[i];
        if (!e.alive) continue;
        if (e.a < adjacency_.size()) adjacency_[e.a].push_back(i);
        if (e.b != e.a && e.b < adjacency_.size()) adjacency_[e.b].push_back(i);
    }
    adjacency_valid_ = true;
}

std::vector<u32> CurveNetwork::edges_at(u32 node) const {
    // A table rather than a scan: it is thrown away whenever an edge could
    // have been rewired (every authoring call, and any edge() handed out to
    // be written to), and made again the first time somebody asks.
    if (!adjacency_valid_ || adjacency_.size() != nodes_.size()) build_adjacency();
    return node < adjacency_.size() ? adjacency_[node] : std::vector<u32>{};
}

const std::vector<u32>& CurveNetwork::connectors_at(u32 node) const {
    return node < node_connectors_.size() ? node_connectors_[node] : kNoIndices;
}

const std::vector<u32>& CurveNetwork::connectors_from(TrackRef track) const {
    const u32 i = track_index(track);
    return i < track_connectors_.size() ? track_connectors_[i] : kNoIndices;
}

const std::vector<u32>& CurveNetwork::connectors_into(TrackRef track) const {
    const u32 i = track_index(track);
    return i < track_feeders_.size() ? track_feeders_[i] : kNoIndices;
}

u32 CurveNetwork::find_connector(TrackRef from, TrackRef to) const {
    for (u32 c : connectors_from(from))
        if (connectors_[c].to == to) return c;
    return kNone;
}

const std::vector<u32>& CurveNetwork::conflicts_of(u32 connector) const {
    return connector < connector_conflicts_.size() ? connector_conflicts_[connector] : kNoIndices;
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

float CurveNetwork::track_start(TrackRef t) const {
    const u32 i = track_index(t);
    return i == kNone ? 0.0f : track_along_[i];
}

bool CurveNetwork::beside(TrackRef from, float distance, TrackRef to, float& out) const {
    const u32 a = track_index(from), b = track_index(to);
    if (a == kNone || b == kNone || from.edge != to.edge) return false;
    if (edges_[from.edge].tracks[from.track].forward != edges_[to.edge].tracks[to.track].forward) return false;
    out = distance + track_along_[a] - track_along_[b];
    return out >= 0 && out <= track_lengths_[b];
}

float CurveNetwork::elevation_at(u32 edge, float t) const {
    if (!valid_edge(edge)) return 0;
    const NetEdge& e = edges_[edge];
    return lerpf(nodes_[e.a].elevation, nodes_[e.b].elevation, clampf(t, 0, 1));
}

float CurveNetwork::grade(u32 edge) const {
    if (!valid_edge(edge)) return 0;
    const NetEdge& e = edges_[edge];
    const float run = e.path.length();
    if (run <= 1e-6f) return 0;
    return (nodes_[e.b].elevation - nodes_[e.a].elevation) / run;
}

float CurveNetwork::grade_along(TrackRef track) const {
    if (!valid_edge(track.edge)) return 0;
    const NetEdge& e = edges_[track.edge];
    const float g = grade(track.edge);
    return track.track < e.tracks.size() && !e.tracks[track.track].forward ? -g : g;
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
        if (rect_distance(edges_[i].path.bounds(), p) > best_d) continue; // cannot beat the best so far
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
        if (rect_distance(track_paths_[i].bounds(), p) > best_d) continue;
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
    r.version = version_;
    const u32 a = track_index(from), b = track_index(to);
    if (a == kNone || b == kNone) return r;
    r.steps.push_back(RouteStep{RouteStep::Kind::Track, a});
    if (a == b) {
        r.found = true;
        return r;
    }
    const GraphPath gp = cache_.get(a, b, version_, [&] {
        return bidirectional ? route_graph_.bidirectional_path(a, b) : route_graph_.shortest_path(a, b);
    });
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
