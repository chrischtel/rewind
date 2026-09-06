#include "rewind/core/input_map.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace rw {

namespace {
const char* const kNames[kButtonCount] = {"up", "down", "left", "right", "a", "b", "x", "y", "l", "r", "start", "select"};
}

const char* button_name(Button b) {
    const u32 bits = static_cast<u32>(b);
    for (u32 i = 0; i < kButtonCount; ++i)
        if (bits == (1u << i)) return kNames[i];
    return "?";
}

bool button_from_name(std::string_view name, Button& out) {
    for (u32 i = 0; i < kButtonCount; ++i)
        if (name == kNames[i]) {
            out = static_cast<Button>(1u << i);
            return true;
        }
    return false;
}

InputBindings InputBindings::defaults() {
    InputBindings b;
    b.buttons = {
        {"up", {"Up", "W"}, {"dpup"}},
        {"down", {"Down", "S"}, {"dpdown"}},
        {"left", {"Left", "A"}, {"dpleft"}},
        {"right", {"Right", "D"}, {"dpright"}},
        {"a", {"Z", "J", "Space"}, {"a"}},
        {"b", {"X", "K", "Left Shift"}, {"b"}},
        {"x", {"C", "L"}, {"x"}},
        {"y", {"V", "I"}, {"y"}},
        {"l", {"Q"}, {"leftshoulder"}},
        {"r", {"E"}, {"rightshoulder"}},
        {"start", {"Return"}, {"start"}},
        {"select", {"Backspace"}, {"back"}},
    };
    return b;
}

ButtonBinding* InputBindings::find(std::string_view button) {
    for (ButtonBinding& b : buttons)
        if (b.button == button) return &b;
    return nullptr;
}

const ButtonBinding* InputBindings::find(std::string_view button) const {
    for (const ButtonBinding& b : buttons)
        if (b.button == button) return &b;
    return nullptr;
}

namespace {
ButtonBinding& entry(InputBindings& bindings, std::string_view button) {
    if (ButtonBinding* b = bindings.find(button)) return *b;
    bindings.buttons.push_back(ButtonBinding{std::string(button), {}, {}});
    return bindings.buttons.back();
}
void add_unique(std::vector<std::string>& list, const std::string& name) {
    if (std::find(list.begin(), list.end(), name) == list.end()) list.push_back(name);
}
void erase(std::vector<std::string>& list, const std::string& name) {
    list.erase(std::remove(list.begin(), list.end(), name), list.end());
}
} // namespace

void InputBindings::add_key(std::string_view button, const std::string& key) { add_unique(entry(*this, button).keys, key); }
void InputBindings::add_gamepad(std::string_view button, const std::string& pad) { add_unique(entry(*this, button).gamepad, pad); }
void InputBindings::remove_key(std::string_view button, const std::string& key) {
    if (ButtonBinding* b = find(button)) erase(b->keys, key);
}
void InputBindings::remove_gamepad(std::string_view button, const std::string& pad) {
    if (ButtonBinding* b = find(button)) erase(b->gamepad, pad);
}

std::string key_name(int scancode) {
    if (scancode < 0 || scancode >= SDL_SCANCODE_COUNT) return {};
    const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    return n ? n : "";
}

int scancode_from_name(const std::string& name) {
    const SDL_Scancode s = SDL_GetScancodeFromName(name.c_str());
    return s == SDL_SCANCODE_UNKNOWN ? -1 : static_cast<int>(s);
}

std::string gamepad_button_name(int button) {
    if (button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT) return {};
    const char* n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(button));
    return n ? n : "";
}

int gamepad_button_from_name(const std::string& name) {
    const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name.c_str());
    return b == SDL_GAMEPAD_BUTTON_INVALID ? -1 : static_cast<int>(b);
}

CompiledBindings compile_bindings(const InputBindings& bindings) {
    CompiledBindings c;
    c.deadzone = bindings.stick_deadzone;
    for (const ButtonBinding& b : bindings.buttons) {
        Button button;
        if (!button_from_name(b.button, button)) {
            c.unknown.push_back("button '" + b.button + "'");
            continue;
        }
        const u32 bit = static_cast<u32>(button);
        for (const std::string& k : b.keys) {
            const int s = scancode_from_name(k);
            if (s < 0 || s >= static_cast<int>(CompiledBindings::kScancodes)) c.unknown.push_back("key '" + k + "'");
            else c.key_mask[s] |= bit;
        }
        for (const std::string& p : b.gamepad) {
            const int g = gamepad_button_from_name(p);
            if (g < 0 || g >= static_cast<int>(CompiledBindings::kPadButtons)) c.unknown.push_back("gamepad button '" + p + "'");
            else c.pad_mask[g] |= bit;
        }
    }
    return c;
}

} // namespace rw
