#pragma once

// What a player changes in an options menu, kept per user rather than per
// project: window mode, volumes, bindings, plus a free-form block for the
// game's own options. Saved as JSON in the OS's per-user folder for the
// app (SDL_GetPrefPath), so it survives reinstalls and never touches the
// game's files.

#include "rewind/audio/audio.h"
#include "rewind/core/input_map.h"
#include "rewind/sim/reflect.h"
#include "rewind/sim/text_format.h"

#include <string>

namespace rw {

struct PlayerSettings {
    bool fullscreen = false;
    bool vsync = true;
    AudioMix audio;
    InputBindings input; ///< empty = the project's bindings
    JsonValue game;      ///< the game's own options: scene_settings-style, apply_json into your struct

    void reflect(Visitor& v) {
        v.field("fullscreen", fullscreen);
        v.field("vsync", vsync);
        v.field("audio", audio);
        v.field("input", input);
    }
};

/// Folder SDL gives this organisation and app for user data, with a
/// trailing separator; empty when the platform has none.
std::string user_data_dir(const std::string& organization, const std::string& app);

/// <user data dir>/settings.json. Returns false when the file is absent or unreadable.
bool load_player_settings(const std::string& organization, const std::string& app, PlayerSettings& out,
                          std::string* error = nullptr);
bool save_player_settings(const std::string& organization, const std::string& app, PlayerSettings& settings,
                          std::string* error = nullptr);

/// Text form with the game block included (reflect leaves it out).
std::string player_settings_to_json(PlayerSettings& settings);
bool player_settings_from_json(const std::string& text, PlayerSettings& out, std::string* error = nullptr);

} // namespace rw
