#pragma once

// Spatial index for "what is near here" questions: a uniform hash grid of
// axis-aligned bounds. Cheap to update every tick for moving things, no
// world size limit, and fast rectangle, circle, segment and nearest
// queries. Header-only, works with any payload (entity handles, indices,
// pointers).
//
// It is derived data: rebuild it from the simulation state after a load or
// rewind rather than saving it. Iteration order of query results is not
// defined; sort by handle if determinism matters for what you do with them.

#include "rewind/core/math.h"
#include "rewind/core/types.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <vector>

namespace rw {

/// Whether segment ab touches the rectangle (slab test).
inline bool segment_intersects_rect(Vec2 a, Vec2 b, const Rect& r) {
    float t0 = 0, t1 = 1;
    const Vec2 d = b - a;
    const float lo[2] = {r.x, r.y}, hi[2] = {r.right(), r.bottom()};
    const float p[2] = {a.x, a.y}, dd[2] = {d.x, d.y};
    for (int i = 0; i < 2; ++i) {
        if (absf(dd[i]) < 1e-9f) {
            if (p[i] < lo[i] || p[i] > hi[i]) return false;
            continue;
        }
        float ta = (lo[i] - p[i]) / dd[i], tb = (hi[i] - p[i]) / dd[i];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}

/// Distance from a point to a rectangle (0 inside).
inline float rect_distance(const Rect& r, Vec2 p) {
    const float dx = std::max({r.x - p.x, 0.0f, p.x - r.right()});
    const float dy = std::max({r.y - p.y, 0.0f, p.y - r.bottom()});
    return std::sqrt(dx * dx + dy * dy);
}

template <class T> class SpatialGrid {
public:
    using Handle = u32;
    static constexpr Handle kInvalid = ~0u;

    explicit SpatialGrid(float cell_size = 64) { set_cell_size(cell_size); }

    /// Cells about the size of a typical object work best. Clears the grid.
    void set_cell_size(float size) {
        cell_ = std::max(size, 1e-3f);
        clear();
    }
    float cell_size() const { return cell_; }

    void clear() {
        entries_.clear();
        free_.clear();
        cells_.clear();
        count_ = 0;
    }

    Handle insert(const T& item, const Rect& bounds) {
        Handle h;
        if (!free_.empty()) {
            h = free_.back();
            free_.pop_back();
            entries_[h] = Entry{item, bounds, true, 0};
        } else {
            h = static_cast<Handle>(entries_.size());
            entries_.push_back(Entry{item, bounds, true, 0});
        }
        link(h, bounds);
        ++count_;
        return h;
    }

    /// Move or resize. Cheap when the item stays within the same cells.
    void update(Handle h, const Rect& bounds) {
        if (!valid(h)) return;
        Entry& e = entries_[h];
        const CellRange before = range(e.bounds), after = range(bounds);
        if (before == after) {
            e.bounds = bounds;
            return;
        }
        unlink(h, e.bounds);
        e.bounds = bounds;
        link(h, bounds);
    }

    void remove(Handle h) {
        if (!valid(h)) return;
        unlink(h, entries_[h].bounds);
        entries_[h].alive = false;
        free_.push_back(h);
        --count_;
    }

    bool valid(Handle h) const { return h < entries_.size() && entries_[h].alive; }
    const T& item(Handle h) const { return entries_[h].item; }
    T& item(Handle h) { return entries_[h].item; }
    const Rect& bounds(Handle h) const { return entries_[h].bounds; }
    usize size() const { return count_; }
    bool empty() const { return count_ == 0; }

    /// Every item whose bounds overlap `area`: fn(handle, item, bounds). Each item once.
    template <class F> void query(const Rect& area, F&& fn) const {
        const u32 stamp = next_stamp();
        const CellRange r = range(area);
        for (i32 cy = r.y0; cy <= r.y1; ++cy)
            for (i32 cx = r.x0; cx <= r.x1; ++cx) {
                auto it = cells_.find(key(cx, cy));
                if (it == cells_.end()) continue;
                for (Handle h : it->second) {
                    const Entry& e = entries_[h];
                    if (e.stamp == stamp) continue;
                    e.stamp = stamp;
                    if (overlaps(e.bounds, area)) fn(h, e.item, e.bounds);
                }
            }
    }

    /// Items whose bounds come within `radius` of `center`.
    template <class F> void query_circle(Vec2 center, float radius, F&& fn) const {
        query(Rect{center.x - radius, center.y - radius, radius * 2, radius * 2}, [&](Handle h, const T& item, const Rect& b) {
            if (rect_distance(b, center) <= radius) fn(h, item, b);
        });
    }

    /// Items whose bounds are crossed by segment ab (line of sight, bullets).
    template <class F> void query_segment(Vec2 a, Vec2 b, F&& fn) const {
        const Rect area{std::min(a.x, b.x), std::min(a.y, b.y), absf(b.x - a.x), absf(b.y - a.y)};
        query(area, [&](Handle h, const T& item, const Rect& r) {
            if (segment_intersects_rect(a, b, r)) fn(h, item, r);
        });
    }

    /// Item at `p` (bounds containing it), or the one whose bounds are
    /// closest within `max_distance`. Ties go to the lower handle.
    std::optional<Handle> nearest(Vec2 p, float max_distance) const {
        if (count_ == 0 || max_distance < 0) return std::nullopt;
        // Grow the search area until something is found, then do one exact
        // pass at that distance so a nearer item in a farther cell wins.
        float radius = cell_;
        Handle best = kInvalid;
        float best_d = 1e30f;
        auto consider = [&](Handle h, const T&, const Rect& b) {
            const float d = rect_distance(b, p);
            if (d < best_d || (d == best_d && h < best)) {
                best_d = d;
                best = h;
            }
        };
        while (true) {
            query_circle(p, std::min(radius, max_distance), consider);
            if (best != kInvalid || radius >= max_distance) break;
            radius *= 2;
        }
        if (best == kInvalid || best_d > max_distance) return std::nullopt;
        if (best_d > 0) query_circle(p, best_d, consider);
        return best;
    }

    /// Bounds of every live item together (empty rect when empty).
    Rect extent() const {
        Rect r{};
        bool first = true;
        for (const Entry& e : entries_) {
            if (!e.alive) continue;
            if (first) {
                r = e.bounds;
                first = false;
                continue;
            }
            const float x0 = std::min(r.x, e.bounds.x), y0 = std::min(r.y, e.bounds.y);
            const float x1 = std::max(r.right(), e.bounds.right()), y1 = std::max(r.bottom(), e.bounds.bottom());
            r = {x0, y0, x1 - x0, y1 - y0};
        }
        return r;
    }

    /// Visit every live item: fn(handle, item, bounds).
    template <class F> void each(F&& fn) const {
        for (usize i = 0; i < entries_.size(); ++i)
            if (entries_[i].alive) fn(static_cast<Handle>(i), entries_[i].item, entries_[i].bounds);
    }

private:
    struct Entry {
        T item;
        Rect bounds;
        bool alive;
        mutable u32 stamp;
    };
    struct CellRange {
        i32 x0, y0, x1, y1;
        bool operator==(const CellRange&) const = default;
    };

    static u64 key(i32 cx, i32 cy) { return (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cy); }
    i32 cell_of(float v) const { return static_cast<i32>(std::floor(v / cell_)); }
    CellRange range(const Rect& r) const {
        return {cell_of(r.x), cell_of(r.y), cell_of(std::max(r.right(), r.x)), cell_of(std::max(r.bottom(), r.y))};
    }
    u32 next_stamp() const {
        if (++stamp_ == 0) {
            for (const Entry& e : entries_) e.stamp = 0;
            stamp_ = 1;
        }
        return stamp_;
    }
    void link(Handle h, const Rect& b) {
        const CellRange r = range(b);
        for (i32 cy = r.y0; cy <= r.y1; ++cy)
            for (i32 cx = r.x0; cx <= r.x1; ++cx) cells_[key(cx, cy)].push_back(h);
    }
    void unlink(Handle h, const Rect& b) {
        const CellRange r = range(b);
        for (i32 cy = r.y0; cy <= r.y1; ++cy)
            for (i32 cx = r.x0; cx <= r.x1; ++cx) {
                auto it = cells_.find(key(cx, cy));
                if (it == cells_.end()) continue;
                std::vector<Handle>& v = it->second;
                auto pos = std::find(v.begin(), v.end(), h);
                if (pos != v.end()) {
                    *pos = v.back();
                    v.pop_back();
                }
                if (v.empty()) cells_.erase(it);
            }
    }

    float cell_ = 64;
    std::vector<Entry> entries_;
    std::vector<Handle> free_;
    std::unordered_map<u64, std::vector<Handle>> cells_;
    usize count_ = 0;
    mutable u32 stamp_ = 0;
};

} // namespace rw
