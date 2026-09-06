#include "rewind/sim/network_agents.h"

#include <algorithm>
#include <cmath>

namespace rw {

namespace {
constexpr float kBig = 1e9f;
constexpr float kSampleTolerance = 0.5f;

/// Speed at which braking at `decel` stops exactly after `gap`.
float stopping_speed(float gap, float decel) { return gap <= 0 ? 0.0f : std::sqrt(2.0f * decel * gap); }
} // namespace

// ---- lifecycle -------------------------------------------------------------------------

u32 NetworkAgents::spawn(const Route& route, float distance, const NetAgentParams& params, u32 tag) {
    NetAgent a;
    a.alive = true;
    a.route = route;
    a.step = 0;
    a.distance = std::max(distance, 0.0f);
    a.params = params;
    a.tag = tag;
    for (u32 i = 0; i < agents_.size(); ++i)
        if (!agents_[i].alive) {
            agents_[i] = a;
            return i;
        }
    agents_.push_back(a);
    return static_cast<u32>(agents_.size() - 1);
}

void NetworkAgents::remove(u32 agent) {
    if (agent < agents_.size()) agents_[agent].alive = false;
}

void NetworkAgents::clear() { agents_.clear(); }

bool NetworkAgents::reroute(u32 agent, const Route& route) {
    if (!valid(agent) || !route.found || route.steps.empty()) return false;
    NetAgent& a = agents_[agent];
    const Piece here = piece_of(a);
    if (here.kind != RouteStep::Kind::Track || route.steps[0].kind != RouteStep::Kind::Track || route.steps[0].index != here.index)
        return false;
    a.route = route;
    a.step = 0;
    a.done = false;
    return true;
}

usize NetworkAgents::count() const {
    usize n = 0;
    for (const NetAgent& a : agents_) n += a.alive ? 1 : 0;
    return n;
}

// ---- pieces -----------------------------------------------------------------------------

NetworkAgents::Piece NetworkAgents::piece_of(const NetAgent& a) const {
    if (a.step >= a.route.steps.size()) return Piece{RouteStep::Kind::Track, ~0u};
    return Piece{a.route.steps[a.step].kind, a.route.steps[a.step].index};
}

const RouteStep* NetworkAgents::current_step(u32 agent) const {
    if (!valid(agent)) return nullptr;
    const NetAgent& a = agents_[agent];
    return a.step < a.route.steps.size() ? &a.route.steps[a.step] : nullptr;
}

float NetworkAgents::piece_len(const CurveNetwork& net, const NetAgent& a, const Piece& p) const {
    switch (p.kind) {
    case RouteStep::Kind::Track: return p.index < net.track_count() ? net.track_length(net.track_ref(p.index)) : 0.0f;
    case RouteStep::Kind::Connector: return p.index < net.connectors().size() ? net.connectors()[p.index].length : 0.0f;
    case RouteStep::Kind::Change: return a.params.change_length;
    }
    return 0.0f;
}

float NetworkAgents::piece_length(const CurveNetwork& net, const NetAgent& a) const { return piece_len(net, a, piece_of(a)); }

std::vector<u32> NetworkAgents::on_connector(u32 connector) const {
    return connector < by_connector_.size() ? by_connector_[connector] : std::vector<u32>{};
}

std::vector<u32> NetworkAgents::on_track(u32 track) const {
    return track < by_track_.size() ? by_track_[track] : std::vector<u32>{};
}

PathPoint NetworkAgents::pose(const CurveNetwork& net, u32 agent) const {
    PathPoint p;
    if (!valid(agent)) return p;
    const NetAgent& a = agents_[agent];
    const Piece here = piece_of(a);
    switch (here.kind) {
    case RouteStep::Kind::Track: {
        if (here.index >= net.track_count()) return p;
        PathSampler s(net.track_path(net.track_ref(here.index)), kSampleTolerance);
        return s.at(a.distance);
    }
    case RouteStep::Kind::Connector: {
        if (here.index >= net.connectors().size()) return p;
        PathSampler s(net.connectors()[here.index].path, kSampleTolerance);
        return s.at(a.distance);
    }
    case RouteStep::Kind::Change: {
        // Between the previous track and the next: blend the two poses.
        if (a.step == 0 || a.step + 1 >= a.route.steps.size() || here.index >= net.links().size()) return p;
        const TrackLink& link = net.links()[here.index];
        PathSampler from(net.track_path(link.from), kSampleTolerance), to(net.track_path(link.to), kSampleTolerance);
        // Distance along the tracks: the change started at `start` (stored as negative stop_distance trick is
        // avoided; instead the change piece runs from the point where the agent left the first track).
        const float start = a.stop_distance < 0 ? 0.0f : a.stop_distance; // reused as the change's start distance
        const float along = start + a.distance;
        const float u = clampf(a.distance / std::max(a.params.change_length, 1e-3f), 0, 1);
        const float blend = u * u * (3 - 2 * u);
        const PathPoint pa = from.at(along), pb = to.at(along);
        p.position = lerp(pa.position, pb.position, blend);
        p.direction = normalize(lerp(pa.direction, pb.direction, blend));
        p.distance = along;
        return p;
    }
    }
    return p;
}

// ---- spacing ------------------------------------------------------------------------------

float NetworkAgents::gap_ahead(const CurveNetwork& net, u32 agent, float lookahead) const {
    const NetAgent& a = agents_[agent];
    // Walk the route from the current piece: the nearest front on the same
    // piece ahead of us, else the first on following pieces.
    float base = 0; // distance from our front to the start of the piece being examined
    for (usize s = a.step; s < a.route.steps.size() && base < lookahead; ++s) {
        const Piece p{a.route.steps[s].kind, a.route.steps[s].index};
        const std::vector<u32>* list = nullptr;
        if (p.kind == RouteStep::Kind::Track && p.index < by_track_.size()) list = &by_track_[p.index];
        else if (p.kind == RouteStep::Kind::Connector && p.index < by_connector_.size()) list = &by_connector_[p.index];
        else if (p.kind == RouteStep::Kind::Change && p.index < by_change_.size()) list = &by_change_[p.index];
        const float len = piece_len(net, a, p);
        if (list) {
            float best = kBig;
            for (u32 other : *list) {
                if (other == agent) continue;
                const NetAgent& o = agents_[other];
                const float tail = o.distance - o.params.length;
                if (s == a.step) {
                    if (o.distance <= a.distance) continue; // behind or level with us
                    if (o.distance == a.distance && other < agent) continue;
                    best = std::min(best, tail - a.distance);
                } else {
                    best = std::min(best, base + tail);
                }
            }
            if (best < kBig) return std::max(best, 0.0f);
        }
        base += s == a.step ? len - a.distance : len;
    }
    return kBig;
}

bool NetworkAgents::may_enter(const CurveNetwork& net, u32 agent, u32 connector) const {
    if (rule_) return rule_(*this, agent, connector);
    // Default: nobody on a connector that crosses this one.
    for (const Conflict& c : net.conflicts()) {
        const u32 other = c.connector_a == connector ? c.connector_b : c.connector_b == connector ? c.connector_a : ~0u;
        if (other == ~0u) continue;
        if (other < by_connector_.size() && !by_connector_[other].empty()) return false;
    }
    return true;
}

// ---- stepping -----------------------------------------------------------------------------

void NetworkAgents::advance(const CurveNetwork& net, u32 agent, float dt) {
    NetAgent& a = agents_[agent];
    if (a.done || a.step >= a.route.steps.size()) {
        a.speed = 0;
        a.done = true;
        return;
    }
    const NetAgentParams& p = a.params;
    const Piece here = piece_of(a);
    const float len = piece_len(net, a, here);
    a.waiting = false;

    // Where must we be able to stop? The end of the route, a stop line
    // before a connector we may not enter yet, or behind the agent ahead.
    float stop_at = kBig; // distance ahead of our front
    const bool last = a.step + 1 >= a.route.steps.size();
    if (last) {
        const float end = a.stop_distance >= 0 && here.kind == RouteStep::Kind::Track ? std::min(a.stop_distance, len) : len;
        stop_at = std::min(stop_at, end - a.distance);
    } else if (here.kind == RouteStep::Kind::Track && a.route.steps[a.step + 1].kind == RouteStep::Kind::Connector) {
        const u32 connector = a.route.steps[a.step + 1].index;
        if (!may_enter(net, agent, connector)) {
            stop_at = std::min(stop_at, len - a.distance);
            a.waiting = true;
        }
    }
    const float lookahead = p.max_speed * p.headway * 2 + p.min_gap + 2 * len;
    const float gap = gap_ahead(net, agent, lookahead);
    if (gap < kBig) stop_at = std::min(stop_at, gap - p.min_gap);

    // Target speed: the slower of free flow, the stopping limit, and a
    // following speed that keeps `headway` seconds behind the leader.
    float target = p.max_speed;
    if (stop_at < kBig) target = std::min(target, stopping_speed(stop_at, p.decel));
    if (gap < kBig && p.headway > 0) target = std::min(target, std::max(gap - p.min_gap, 0.0f) / p.headway);
    if (target > a.speed) a.speed = std::min(target, a.speed + p.accel * dt);
    else a.speed = std::max(target, a.speed - p.decel * dt);
    if (a.speed < 0.25f && target < 0.25f) a.speed = 0; // creeping the last fraction of a unit: just stand

    float move = a.speed * dt;
    if (stop_at < kBig) move = std::min(move, std::max(stop_at, 0.0f));
    a.distance += move;

    // Reached a stop distance on the last track: park there.
    if (last && here.kind == RouteStep::Kind::Track && a.stop_distance >= 0 && a.distance >= std::min(a.stop_distance, len) - 1e-4f) {
        a.distance = std::min(a.stop_distance, len);
        a.speed = 0;
        a.done = true;
        return;
    }

    // Piece transitions (at most one per tick keeps it simple and stable).
    if (a.distance >= len - 1e-4f) {
        if (last) {
            a.distance = len;
            a.speed = 0;
            a.done = true;
            return;
        }
        const RouteStep& next = a.route.steps[a.step + 1];
        if (next.kind == RouteStep::Kind::Connector && a.waiting) {
            a.distance = len;
            a.speed = 0;
            return;
        }
        const float carry = a.distance - len;
        if (next.kind == RouteStep::Kind::Change) {
            // Remember where on the track the change begins (reuse stop_distance while changing).
            a.stop_distance = len;
            a.step += 1;
            a.distance = carry;
        } else if (here.kind == RouteStep::Kind::Change) {
            // Done changing: continue on the new track from where the change ends.
            const float start = a.stop_distance < 0 ? 0.0f : a.stop_distance;
            a.stop_distance = -1;
            a.step += 1;
            a.distance = start + p.change_length + carry;
        } else {
            a.step += 1;
            a.distance = carry;
        }
    }
}

void NetworkAgents::tick(const CurveNetwork& net, float dt) {
    // Index who is where, front-most first, ties by agent index.
    by_track_.assign(net.track_count(), {});
    by_connector_.assign(net.connectors().size(), {});
    by_change_.assign(net.links().size(), {});
    for (u32 i = 0; i < agents_.size(); ++i) {
        const NetAgent& a = agents_[i];
        if (!a.alive) continue; // parked (done) agents still take up room
        const Piece p = piece_of(a);
        if (p.kind == RouteStep::Kind::Track && p.index < by_track_.size()) by_track_[p.index].push_back(i);
        else if (p.kind == RouteStep::Kind::Connector && p.index < by_connector_.size()) by_connector_[p.index].push_back(i);
        else if (p.kind == RouteStep::Kind::Change && p.index < by_change_.size()) by_change_[p.index].push_back(i);
    }
    auto by_front = [&](std::vector<u32>& list) {
        std::sort(list.begin(), list.end(), [&](u32 x, u32 y) {
            return agents_[x].distance > agents_[y].distance || (agents_[x].distance == agents_[y].distance && x < y);
        });
    };
    for (auto& l : by_track_) by_front(l);
    for (auto& l : by_connector_) by_front(l);
    for (auto& l : by_change_) by_front(l);

    for (u32 i = 0; i < agents_.size(); ++i)
        if (agents_[i].alive && !agents_[i].done) advance(net, i, dt);
}

} // namespace rw
