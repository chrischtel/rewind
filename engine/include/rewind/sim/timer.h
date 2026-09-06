#pragma once

// Timers, cooldowns and state machines as plain reflected data: they live
// in your game state, so they rewind, replay, save and show in the
// inspector like everything else. Drive them from tick() with the fixed dt.

#include "rewind/core/types.h"
#include "rewind/sim/reflect.h"

namespace rw {

/// Counts up to `duration`. tick() returns true on the tick it fires; a
/// looping timer keeps the overshoot so long durations stay accurate.
struct Timer {
    float duration = 1;
    float elapsed = 0;
    bool running = false;
    bool loop = false;

    void start(float seconds, bool looping = false) {
        duration = seconds;
        loop = looping;
        elapsed = 0;
        running = true;
    }
    void stop() { running = false; }
    void reset() { elapsed = 0; }
    bool tick(float dt) {
        if (!running) return false;
        elapsed += dt;
        if (elapsed < duration) return false;
        if (loop) elapsed -= duration;
        else running = false;
        return true;
    }
    float progress() const { return duration > 0 ? (elapsed < duration ? elapsed / duration : 1.0f) : 1.0f; }
    float remaining() const { return duration > elapsed ? duration - elapsed : 0.0f; }
    bool finished() const { return !running && elapsed >= duration; }

    void reflect(Visitor& v) {
        v.field("duration", duration, {.min = 0});
        v.field("elapsed", elapsed, {.min = 0});
        v.field("running", running);
        v.field("loop", loop);
    }
};

/// Time until something may happen again. use() starts it, ready() asks.
struct Cooldown {
    float remaining = 0;

    bool ready() const { return remaining <= 0; }
    void use(float seconds) { remaining = seconds; }
    /// Returns true when the cooldown ended this tick.
    bool tick(float dt) {
        if (remaining <= 0) return false;
        remaining -= dt;
        if (remaining > 0) return false;
        remaining = 0;
        return true;
    }
    void clear() { remaining = 0; }

    void reflect(Visitor& v) { v.field("remaining", remaining, {.min = 0}); }
};

/// A state as data: current and previous state, time in the current one,
/// and a one-tick "just entered" flag. Pass state names for the inspector.
/// Logic stays in your code: `switch (fsm.current)` in tick().
template <class State> struct StateMachine {
    State current{};
    State previous{};
    float time_in_state = 0;
    u32 transitions = 0;
    bool entered = true; ///< true from change() until the next tick()

    StateMachine() = default;
    explicit StateMachine(State initial, const char* const* names = nullptr, u32 name_count = 0)
        : current(initial), previous(initial), names_(names), name_count_(name_count) {}

    /// Switch states. Returns false when already there (no re-entry).
    bool change(State next) {
        if (next == current) return false;
        previous = current;
        current = next;
        time_in_state = 0;
        entered = true;
        ++transitions;
        return true;
    }
    /// Re-enter the current state (resets the timer, sets `entered`).
    void restart() {
        time_in_state = 0;
        entered = true;
        ++transitions;
    }
    void tick(float dt) {
        time_in_state += dt;
        entered = false;
    }
    bool is(State s) const { return current == s; }
    bool was(State s) const { return previous == s; }
    /// Name for the inspector, or "" without names.
    const char* name(State s) const {
        const usize i = static_cast<usize>(s);
        return names_ && i < name_count_ ? names_[i] : "";
    }
    void set_names(const char* const* names, u32 count) {
        names_ = names;
        name_count_ = count;
    }

    void reflect(Visitor& v) {
        v.field("current", current, {.names = names_, .name_count = name_count_});
        v.field("previous", previous, {.read_only = true, .names = names_, .name_count = name_count_});
        v.field("time_in_state", time_in_state, {.min = 0});
        v.field("transitions", transitions, {.read_only = true});
        v.field("entered", entered);
    }

private:
    const char* const* names_ = nullptr;
    u32 name_count_ = 0;
};

} // namespace rw
