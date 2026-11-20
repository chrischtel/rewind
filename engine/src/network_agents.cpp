#include "rewind/sim/network_agents.h"

#include <algorithm>
#include <cmath>

namespace rw {

namespace {
constexpr float kBig = 1e9f;
constexpr float kSampleTolerance = 0.5f;
/// A stop line is treated as something standing this far beyond it, so the
/// driver model comes to rest on the line itself.
constexpr float kLineStandoff = 0.5f;

/// Speed at which braking at `decel` stops exactly after `gap`.
float stopping_speed(float gap, float decel) { return gap <= 0 ? 0.0f : std::sqrt(2.0f * decel * gap); }

/// The Intelligent Driver Model: acceleration towards the wanted speed `v0`,
/// less a braking term that grows as the gap falls below the one wanted at
/// this speed and this rate of closing.
float idm(float v, float v0, float a, float b, float s0, float headway, float exponent, float gap, float lead_speed) {
    const float free = v0 > 1e-3f ? 1.0f - std::pow(std::max(v, 0.0f) / v0, exponent) : -1.0f;
    if (gap >= kBig) return a * free;
    const float closing = v * (v - lead_speed) / (2.0f * std::sqrt(a * b));
    const float wanted = s0 + std::max(0.0f, v * headway + closing);
    const float ratio = wanted / std::max(gap, 0.01f);
    return a * (free - ratio * ratio);
}
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

namespace {
/// Where a changing agent is on each of its two tracks. Both run the same
/// way on one edge, so one scale along the edge serves for both; the track
/// it is moving to may begin further along, hence the shift.
struct Across {
    TrackRef from, to;
    float on_from = 0, on_to = 0;
    bool ok = false;
};
Across across(const CurveNetwork& net, const NetAgent& a, u32 link) {
    Across x;
    if (link >= net.links().size()) return x;
    x.from = net.links()[link].from;
    x.to = net.links()[link].to;
    x.on_from = a.change_start + a.distance;
    x.on_to = x.on_from + net.track_start(x.from) - net.track_start(x.to);
    x.ok = true;
    return x;
}
} // namespace

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
        // Between the track it left and the one it is joining: blend the two poses.
        const Across x = across(net, a, here.index);
        if (!x.ok) return p;
        PathSampler from(net.track_path(x.from), kSampleTolerance), to(net.track_path(x.to), kSampleTolerance);
        const float u = clampf(a.distance / std::max(a.params.change_length, 1e-3f), 0, 1);
        const float blend = u * u * (3 - 2 * u);
        const PathPoint pa = from.at(x.on_from), pb = to.at(x.on_to);
        p.position = lerp(pa.position, pb.position, blend);
        p.direction = normalize(lerp(pa.direction, pb.direction, blend));
        p.distance = x.on_from;
        return p;
    }
    }
    return p;
}

// ---- spacing ------------------------------------------------------------------------------

/// The nearest thing ahead of `front` on a track: agents on it, and whatever
/// else takes up room there. `base` is how far our own front is from the
/// track's start when it is a later piece of the route; `all_ahead` then says
/// everything on it counts.
void NetworkAgents::scan_track(u32 track, float front, bool all_ahead, float base, u32 self, Lead& best) const {
    const auto consider = [&](float gap, float speed, u32 who) {
        if (gap < best.gap) best = Lead{gap, speed, who};
    };
    if (track < by_track_.size())
        for (u32 other : by_track_[track]) {
            if (other == self) continue;
            const NetAgent& o = agents_[other];
            if (!all_ahead && o.distance <= front) continue; // behind or level with us
            consider(base + o.distance - o.params.length - (all_ahead ? 0.0f : front), o.speed, other);
        }
    if (track < slots_.size())
        for (const Slot& s : slots_[track]) {
            if (s.agent == self) continue;
            if (!all_ahead && s.front <= front) continue;
            const float tail = s.front - s.length;
            // One that is only waiting to come in is given room by those
            // wholly behind it; anyone alongside drives on and lets it in after.
            if (s.soft && !all_ahead && tail < front) continue;
            consider(base + tail - (all_ahead ? 0.0f : front), s.speed, s.agent);
        }
}

NetworkAgents::Lead NetworkAgents::lead_ahead(const CurveNetwork& net, u32 agent, float lookahead) const {
    const NetAgent& a = agents_[agent];
    Lead best;
    float base = 0; // distance from our front to the start of the piece being examined
    usize s = a.step;

    // The piece we are on.
    const Piece here = piece_of(a);
    if (here.kind == RouteStep::Kind::Change) {
        // Half in one track and half in the other: both count, and the route
        // carries on from where we are on the one we are joining.
        const Across x = across(net, a, here.index);
        if (!x.ok) return best;
        scan_track(net.track_index(x.from), x.on_from, false, 0, agent, best);
        scan_track(net.track_index(x.to), x.on_to, false, 0, agent, best);
        if (best.gap < kBig) {
            best.gap = std::max(best.gap, 0.0f);
            return best;
        }
        base = net.track_length(x.to) - x.on_to;
        s = a.step + 2; // past the track we are joining
    } else if (here.kind == RouteStep::Kind::Track) {
        scan_track(here.index, a.distance, false, 0, agent, best);
        if (best.gap < kBig) {
            best.gap = std::max(best.gap, 0.0f);
            return best;
        }
        base = piece_len(net, a, here) - a.distance;
        s = a.step + 1;
    } else {
        if (here.index < by_connector_.size())
            for (u32 other : by_connector_[here.index]) {
                if (other == agent) continue;
                const NetAgent& o = agents_[other];
                if (o.distance <= a.distance) continue;
                const float gap = o.distance - o.params.length - a.distance;
                if (gap < best.gap) best = Lead{gap, o.speed, other};
            }
        if (best.gap < kBig) {
            best.gap = std::max(best.gap, 0.0f);
            return best;
        }
        base = piece_len(net, a, here) - a.distance;
        s = a.step + 1;
    }

    // Then along the route, piece by piece.
    for (; s < a.route.steps.size() && base < lookahead; ++s) {
        const Piece p{a.route.steps[s].kind, a.route.steps[s].index};
        // A change still to come is made somewhere along the track before
        // it, not at its end: what lies beyond is looked at when it is made.
        if (p.kind == RouteStep::Kind::Change) break;
        if (p.kind == RouteStep::Kind::Track) {
            scan_track(p.index, 0, true, base, agent, best);
        } else if (p.index < by_connector_.size()) {
            for (u32 other : by_connector_[p.index]) {
                if (other == agent) continue;
                const NetAgent& o = agents_[other];
                const float gap = base + o.distance - o.params.length;
                if (gap < best.gap) best = Lead{gap, o.speed, other};
            }
        }
        if (best.gap < kBig) {
            best.gap = std::max(best.gap, 0.0f);
            return best;
        }
        base += piece_len(net, a, p);
    }
    return best;
}

NetworkAgents::Lead NetworkAgents::lead_of(const CurveNetwork& net, u32 agent) const {
    if (!valid(agent) || agents_[agent].done) return {};
    const NetAgentParams& p = agents_[agent].params;
    return lead_ahead(net, agent, p.max_speed * p.headway * 2 + p.min_gap + 2 * piece_length(net, agents_[agent]));
}

bool NetworkAgents::may_enter(const CurveNetwork& net, u32 agent, u32 connector) const {
    if (rule_) return rule_(*this, agent, connector);
    // Default: nobody on a connector that crosses this one.
    for (u32 index : net.conflicts_of(connector)) {
        const Conflict& c = net.conflicts()[index];
        if (c.kind != ConflictKind::Crossing) continue; // merges are settled by the gap on the track ahead
        const u32 other = c.connector_a == connector ? c.connector_b : c.connector_a;
        if (other < by_connector_.size() && !by_connector_[other].empty()) return false;
    }
    return true;
}

// ---- changing tracks ------------------------------------------------------------------------

/// Is there room for `self` in a track with its front at `front`? Enough in
/// front to stand behind whoever is there, and enough behind that whoever is
/// coming does not have to brake hard for it.
bool NetworkAgents::gap_beside(u32 track, float front, u32 self) const {
    const NetAgent& a = agents_[self];
    const NetAgentParams& p = a.params;
    const float tail = front - p.length;
    // At a crawl half the standing gap will do, in front and behind: a queue
    // settles a hair inside its standing gap, and a merge into one must not
    // wait for a hand's breadth that will never open. At speed the gap grows
    // with how fast it is closing.
    const float headway = std::max(p.headway, 0.5f);
    const auto fits = [&](float other_front, float other_length, float other_speed) {
        if (other_front > front) { // it is ahead
            const float standing = p.min_gap * (a.speed < 0.5f ? 0.5f : 1.0f);
            const float closing = std::max(a.speed - other_speed, 0.0f);
            return other_front - other_length - front >= standing + closing * headway * 0.5f;
        }
        const float standing = p.min_gap * (other_speed < 0.5f ? 0.5f : 1.0f); // it is behind
        const float closing = std::max(other_speed - a.speed, 0.0f);
        return tail - other_front >= standing + closing * headway;
    };
    if (track < by_track_.size())
        for (u32 other : by_track_[track]) {
            if (other == self) continue;
            const NetAgent& o = agents_[other];
            if (!fits(o.distance, o.params.length, o.speed)) return false;
        }
    if (track < slots_.size())
        for (const Slot& s : slots_[track]) {
            if (s.agent == self) continue;
            // Two waiting for the same gap: the lower index goes first, or
            // each would wait for the other for ever.
            if (s.soft && s.agent > self) continue;
            if (!fits(s.front, s.length, s.speed)) return false;
        }
    return true;
}

/// Start the change the route asks for next, if the track beside exists here
/// and has a gap. On success the agent is on the change piece.
bool NetworkAgents::try_change(const CurveNetwork& net, u32 agent) {
    NetAgent& a = agents_[agent];
    const u32 link = a.route.steps[a.step + 1].index;
    if (link >= net.links().size()) return false;
    const TrackRef from = net.links()[link].from, to = net.links()[link].to;
    const float from_len = net.track_length(from), to_len = net.track_length(to);
    const bool at_end = a.distance >= from_len - 0.05f;
    float beside = 0;
    if (!net.beside(from, a.distance, to, beside)) {
        // The other track has not begun yet, or has already ended. At the very
        // end of our own there is nothing left to wait for: go across there.
        if (!at_end) return false;
        beside = clampf(a.distance + net.track_start(from) - net.track_start(to), 0, to_len);
    }
    // The change has to fit on the track it ends on, unless this is the last chance.
    if (!at_end && beside + a.params.change_length > to_len) return false;
    const u32 target = net.track_index(to);
    if (!gap_beside(target, beside, agent)) return false;

    a.change_start = a.distance;
    a.step += 1;
    a.distance = 0;
    // From this moment it takes up room in both tracks, for everyone who has
    // still to move this tick as well.
    const u32 origin = net.track_index(from);
    if (origin < slots_.size()) slots_[origin].push_back(Slot{agent, a.change_start, a.params.length, a.speed, false});
    if (target < slots_.size()) slots_[target].push_back(Slot{agent, beside, a.params.length, a.speed, false});
    return true;
}

// ---- stepping -----------------------------------------------------------------------------

MotionLimits NetworkAgents::limits_of(const CurveNetwork& net, u32 agent) const {
    const NetAgentParams& p = agents_[agent].params;
    const MotionLimits own{p.accel, p.decel, p.max_speed};
    if (!motion_) return own;
    MotionLimits out = motion_(*this, net, agent, own);
    // The game may take the drive away, but not the brakes: without a decel
    // the stopping-distance arithmetic below divides the agent into a wall.
    if (!(out.decel > 0)) out.decel = own.decel > 0 ? own.decel : 1.0f;
    if (!(out.max_speed > 0)) out.max_speed = 0;
    return out;
}

void NetworkAgents::advance(const CurveNetwork& net, u32 agent, float dt) {
    NetAgent& a = agents_[agent];
    if (a.done || a.step >= a.route.steps.size()) {
        a.speed = 0;
        a.accel = 0;
        a.done = true;
        return;
    }
    const NetAgentParams& p = a.params;
    const MotionLimits lim = limits_of(net, agent);
    a.waiting = false;

    // A change of track the route asks for next is made as soon as it can be.
    // Until then the agent may run on as far as the last place from which the
    // change still fits, and holds there for a gap.
    float line_at = kBig; // distance ahead of our front to something we must stop at
    if (piece_of(a).kind == RouteStep::Kind::Track && a.step + 1 < a.route.steps.size() &&
        a.route.steps[a.step + 1].kind == RouteStep::Kind::Change && !try_change(net, agent)) {
        const u32 link = a.route.steps[a.step + 1].index;
        if (link < net.links().size()) {
            const TrackRef from = net.links()[link].from, to = net.links()[link].to;
            const float from_len = net.track_length(from);
            float latest = net.track_length(to) - p.change_length + net.track_start(to) - net.track_start(from);
            latest = std::min(latest, from_len);
            // Already past it (or the other track is too short for a whole
            // change): the end of our own track is all that is left.
            if (latest < a.distance - 0.5f) latest = from_len;
            line_at = std::max(latest - a.distance, 0.0f);
            if (line_at < p.min_gap + 1.0f) a.waiting = true; // those behind in the other track will let it in
        }
    }

    const Piece here = piece_of(a);
    const float len = piece_len(net, a, here);

    // Where must we be able to stop? The end of the route, or a stop line
    // before a connector we may not enter yet. While changing tracks these are
    // measured on the track being joined.
    float on_track = a.distance, track_len = len;
    usize track_step = a.step;
    bool on_a_track = here.kind == RouteStep::Kind::Track;
    if (here.kind == RouteStep::Kind::Change) {
        const Across x = across(net, a, here.index);
        if (x.ok && a.step + 1 < a.route.steps.size()) {
            on_track = x.on_to;
            track_len = net.track_length(x.to);
            track_step = a.step + 1;
            on_a_track = true;
        }
    }
    const bool last = a.step + 1 >= a.route.steps.size();
    const bool track_last = track_step + 1 >= a.route.steps.size();
    if (last) {
        const float end = a.stop_distance >= 0 && here.kind == RouteStep::Kind::Track ? std::min(a.stop_distance, len) : len;
        line_at = std::min(line_at, end - a.distance);
    } else if (on_a_track && track_last) {
        const float end = a.stop_distance >= 0 ? std::min(a.stop_distance, track_len) : track_len;
        line_at = std::min(line_at, end - on_track);
    } else if (on_a_track && a.route.steps[track_step + 1].kind == RouteStep::Kind::Connector) {
        const u32 connector = a.route.steps[track_step + 1].index;
        if (!may_enter(net, agent, connector)) {
            line_at = std::min(line_at, track_len - on_track);
            a.waiting = true;
        }
    }

    const float lookahead = p.max_speed * p.headway * 2 + p.min_gap + 2 * len;
    const Lead lead = lead_ahead(net, agent, lookahead);
    const float gap = lead.gap;
    const float was = a.speed;
    float move = 0;
    float chosen = 0; // the acceleration the driver chose, for the Idm

    if (p.model == FollowModel::Idm) {
        // Respond to whoever is ahead, and to a line as to something standing
        // just beyond it; take the more cautious of the two.
        const float a_max = std::max(lim.accel, 0.05f);
        float acc = idm(was, lim.max_speed, a_max, lim.decel, p.min_gap, p.headway, p.exponent, gap, lead.speed);
        if (line_at < kBig)
            acc = std::min(acc, idm(was, lim.max_speed, a_max, lim.decel, kLineStandoff, 0.0f, p.exponent,
                                    std::max(line_at, 0.0f) + kLineStandoff, 0.0f));
        // A rule that takes the drive away (too much load for the hill) wins
        // over wanting to go; it never stops the braking.
        if (lim.accel <= 0) acc = std::min(acc, lim.accel);
        const float hardest = p.max_brake > 0 ? p.max_brake : lim.decel * 3.0f;
        acc = std::max(acc, -hardest);
        chosen = acc;
        a.speed = std::max(was + acc * dt, 0.0f);
        move = a.speed * dt;
        // Whatever the model says, never into the one ahead and never over a line.
        float room = line_at;
        if (gap < kBig) room = std::min(room, gap - 0.05f);
        if (room < kBig && move > std::max(room, 0.0f)) {
            move = std::max(room, 0.0f);
            a.speed = dt > 0 ? move / dt : 0.0f;
        }
        // The model closes on a line ever more slowly; the last hand's breadth is just taken.
        if (line_at < 0.25f && a.speed < 0.5f) {
            move = std::max(line_at, 0.0f);
            a.speed = 0;
        }
    } else {
        float stop_at = line_at;
        if (gap < kBig) stop_at = std::min(stop_at, gap - p.min_gap);

        // Target speed: the slower of free flow, the stopping limit, and a
        // following speed that keeps `headway` seconds behind the leader.
        float target = lim.max_speed;
        if (stop_at < kBig) target = std::min(target, stopping_speed(stop_at, lim.decel));
        if (gap < kBig && p.headway > 0) target = std::min(target, std::max(gap - p.min_gap, 0.0f) / p.headway);
        if (target > a.speed) a.speed = std::min(target, a.speed + lim.accel * dt);
        else a.speed = std::max(target, a.speed - lim.decel * dt);
        // A negative accel is the rule saying the agent cannot hold its speed
        // here at all -- too much load for the hill. That has to bite even when
        // it is already going no faster than it wanted to, so it is applied
        // against the speed the tick started at rather than against the target.
        if (lim.accel < 0) a.speed = std::min(a.speed, was + lim.accel * dt);
        if (a.speed < 0) a.speed = 0; // slowing to a stand, never reversing
        if (a.speed < 0.25f && target < 0.25f) a.speed = 0; // creeping the last fraction of a unit: just stand

        move = a.speed * dt;
        if (stop_at < kBig) move = std::min(move, std::max(stop_at, 0.0f));
    }
    // What the driver did, not what the arithmetic of standing still on a
    // line did to the last few centimetres a second.
    if (p.model == FollowModel::Idm) a.accel = a.speed > 0 || chosen > 0 ? chosen : 0.0f;
    else a.accel = dt > 0 ? (a.speed - was) / dt : 0.0f;
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
        if (here.kind == RouteStep::Kind::Track && (next.kind == RouteStep::Kind::Change || a.waiting)) {
            // Held at the end: before a connector it may not enter, or for a
            // gap in the track beside.
            a.distance = len;
            a.speed = 0;
            return;
        }
        if (here.kind == RouteStep::Kind::Change) {
            // Done changing: carry on along the new track from where the change ends.
            const Across x = across(net, a, here.index);
            a.step += 1;
            a.distance = x.ok ? std::max(x.on_to, 0.0f) : 0.0f;
            a.change_start = 0;
        } else {
            a.step += 1;
            a.distance -= len;
        }
    }
}

void NetworkAgents::tick(const CurveNetwork& net, float dt) {
    // Index who is where, front-most first, ties by agent index. The lists
    // keep their memory from tick to tick.
    const auto reset = [](auto& lists, usize n) {
        if (lists.size() != n) lists.assign(n, {});
        else
            for (auto& l : lists) l.clear();
    };
    reset(by_track_, net.track_count());
    reset(by_connector_, net.connectors().size());
    reset(by_change_, net.links().size());
    reset(slots_, net.track_count());
    for (u32 i = 0; i < agents_.size(); ++i) {
        const NetAgent& a = agents_[i];
        if (!a.alive) continue; // parked (done) agents still take up room
        const Piece p = piece_of(a);
        if (p.kind == RouteStep::Kind::Track && p.index < by_track_.size()) {
            by_track_[p.index].push_back(i);
            // Held for a gap in the track beside: stand in it as a marker, so
            // whoever comes up behind there stops short and lets it in.
            if (a.waiting && !a.done && a.step + 1 < a.route.steps.size() &&
                a.route.steps[a.step + 1].kind == RouteStep::Kind::Change) {
                const u32 link = a.route.steps[a.step + 1].index;
                if (link < net.links().size()) {
                    const TrackRef from = net.links()[link].from, to = net.links()[link].to;
                    const u32 target = net.track_index(to);
                    const float beside = clampf(a.distance + net.track_start(from) - net.track_start(to), 0,
                                                net.track_length(to));
                    if (target < slots_.size()) slots_[target].push_back(Slot{i, beside, a.params.length, a.speed, true});
                }
            }
        } else if (p.kind == RouteStep::Kind::Connector && p.index < by_connector_.size()) {
            by_connector_[p.index].push_back(i);
        } else if (p.kind == RouteStep::Kind::Change && p.index < by_change_.size()) {
            by_change_[p.index].push_back(i);
            // Half way across: in both tracks at once.
            const Across x = across(net, a, p.index);
            const u32 from = net.track_index(x.from), to = net.track_index(x.to);
            if (from < slots_.size()) slots_[from].push_back(Slot{i, x.on_from, a.params.length, a.speed, false});
            if (to < slots_.size()) slots_[to].push_back(Slot{i, x.on_to, a.params.length, a.speed, false});
        }
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
