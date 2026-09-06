#pragma once

#include "rewind/core/types.h"
#include "rewind/sim/reflect.h"

#include <type_traits>

namespace rw {

/// Abstract controller layout. The engine maps keyboard and gamepad onto this;
/// the simulation only ever sees an InputFrame, never raw device state.
enum class Button : u32 {
    Up = 1u << 0,
    Down = 1u << 1,
    Left = 1u << 2,
    Right = 1u << 3,
    A = 1u << 4,
    B = 1u << 5,
    X = 1u << 6,
    Y = 1u << 7,
    L = 1u << 8,
    R = 1u << 9,
    Start = 1u << 10,
    Select = 1u << 11,
    MouseLeft = 1u << 12,
    MouseRight = 1u << 13,
    MouseMiddle = 1u << 14,
};

/// Everything the simulation is allowed to know about the player for one tick.
/// Small and trivially copyable on purpose: one of these is stored per tick of
/// history, and later it is what goes over the network.
struct InputFrame {
    u32 buttons = 0;
    i16 axis_x = 0; ///< -32767..32767, left stick or derived from the d-pad
    i16 axis_y = 0;
    i16 mouse_x = 0; ///< In render-target pixels (pixel mode aware). Negative = outside.
    i16 mouse_y = 0;
    i8 wheel = 0;    ///< Mouse wheel clicks this tick, positive = away from the user. Only the first tick of a frame gets them.
    i8 pad_ = 0;

    bool down(Button b) const { return (buttons & static_cast<u32>(b)) != 0; }

    void set(Button b, bool on) {
        if (on)
            buttons |= static_cast<u32>(b);
        else
            buttons &= ~static_cast<u32>(b);
    }

    bool operator==(const InputFrame&) const = default;

    void reflect(Visitor& v) {
        v.field("buttons", buttons, {.tooltip = "bitmask, see rw::Button"});
        v.field("axis_x", axis_x, {.min = -32767, .max = 32767});
        v.field("axis_y", axis_y, {.min = -32767, .max = 32767});
        v.field("mouse_x", mouse_x);
        v.field("mouse_y", mouse_y);
        v.field("wheel", wheel);
    }
};

static_assert(std::is_trivially_copyable_v<InputFrame>);

constexpr u32 kMaxPlayers = 4;

/// Input of every player for one tick. Player 0 is the only one in a
/// single-player game; a networked session fills in the others.
struct InputSet {
    InputFrame players[kMaxPlayers]{};
    u8 count = 1;

    InputSet() = default;
    explicit InputSet(const InputFrame& single) : count(1) { players[0] = single; }

    const InputFrame& operator[](usize i) const { return players[i < kMaxPlayers ? i : 0]; }
    InputFrame& operator[](usize i) { return players[i < kMaxPlayers ? i : 0]; }
    bool operator==(const InputSet&) const = default;
};

static_assert(std::is_trivially_copyable_v<InputSet>);

/// Buttons that are down in `now` but were not in `prev`.
inline u32 pressed(const InputFrame& now, const InputFrame& prev) {
    return now.buttons & ~prev.buttons;
}

inline bool pressed(const InputFrame& now, const InputFrame& prev, Button b) {
    return (pressed(now, prev) & static_cast<u32>(b)) != 0;
}

} // namespace rw
