#pragma once

// Input bindings: which keys and gamepad buttons drive each abstract
// Button. Stored as names, so project.json and a player's settings file
// stay readable ("up": ["Up", "W"], "a": ["Z", "Space"]). Compiled into
// lookup tables for sampling.

#include "rewind/core/types.h"
#include "rewind/sim/input.h"
#include "rewind/sim/reflect.h"

#include <string>
#include <vector>

namespace rw {

struct ButtonBinding {
    std::string button;               ///< "up", "down", "left", "right", "a", "b", "x", "y", "l", "r", "start", "select"
    std::vector<std::string> keys;    ///< SDL key names: "W", "Space", "Left", "Return"...
    std::vector<std::string> gamepad; ///< SDL gamepad button names: "a", "b", "x", "y", "dpup", "leftshoulder", "start", "back"...

    void reflect(Visitor& v) {
        v.field("button", button);
        v.field("keys", keys);
        v.field("gamepad", gamepad);
    }
};

struct InputBindings {
    std::vector<ButtonBinding> buttons;
    float stick_deadzone = 0.25f; ///< fraction of full deflection ignored on the left stick

    /// Arrows and WASD, Z/J/Space, X/K/Shift, C/L, V/I, Q, E, Return, Backspace; the usual pad layout.
    static InputBindings defaults();
    bool empty() const { return buttons.empty(); }
    ButtonBinding* find(std::string_view button);
    const ButtonBinding* find(std::string_view button) const;
    /// Add a key or pad name to a button (creating the entry), ignoring duplicates.
    void add_key(std::string_view button, const std::string& key);
    void add_gamepad(std::string_view button, const std::string& pad);
    void remove_key(std::string_view button, const std::string& key);
    void remove_gamepad(std::string_view button, const std::string& pad);

    void reflect(Visitor& v) {
        v.field("buttons", buttons);
        v.field("stick_deadzone", stick_deadzone, {.min = 0, .max = 0.9f});
    }
};

/// Names of the twelve abstract buttons, in bit order.
const char* button_name(Button b);
/// Parse a button name; false when unknown.
bool button_from_name(std::string_view name, Button& out);
constexpr u32 kButtonCount = 12;

/// Bindings resolved to SDL scancodes and gamepad button indices.
struct CompiledBindings {
    static constexpr u32 kScancodes = 512;
    static constexpr u32 kPadButtons = 32;
    u32 key_mask[kScancodes] = {}; ///< Button bits per scancode
    u32 pad_mask[kPadButtons] = {};
    float deadzone = 0.25f;
    std::vector<std::string> unknown; ///< names that did not resolve, for diagnostics
};

CompiledBindings compile_bindings(const InputBindings& bindings);

/// Display names SDL uses, for rebinding UIs.
std::string key_name(int scancode);
int scancode_from_name(const std::string& name); ///< -1 when unknown
std::string gamepad_button_name(int button);
int gamepad_button_from_name(const std::string& name); ///< -1 when unknown

} // namespace rw
