#include "rewind/core/player_settings.h"

#include <SDL3/SDL.h>

namespace rw {

std::string user_data_dir(const std::string& organization, const std::string& app) {
    char* path = SDL_GetPrefPath(organization.c_str(), app.c_str());
    std::string dir = path ? path : "";
    if (path) SDL_free(path);
    return dir;
}

std::string player_settings_to_json(PlayerSettings& settings) {
    JsonValue doc = to_json_value(settings);
    if (settings.game.type != JsonValue::Type::Null) doc.members.emplace_back("game", settings.game);
    return write_json(doc);
}

bool player_settings_from_json(const std::string& text, PlayerSettings& out, std::string* error) {
    JsonValue doc;
    std::string err;
    if (!parse_json(text, doc, err)) {
        if (error) *error = err;
        return false;
    }
    out = PlayerSettings{};
    apply_json(doc, out);
    if (const JsonValue* game = doc.find("game")) out.game = *game;
    return true;
}

bool load_player_settings(const std::string& organization, const std::string& app, PlayerSettings& out, std::string* error) {
    const std::string dir = user_data_dir(organization, app);
    if (dir.empty()) {
        if (error) *error = "no user data folder on this platform";
        return false;
    }
    std::string text;
    if (!read_text_file(dir + "settings.json", text, error)) return false;
    return player_settings_from_json(text, out, error);
}

bool save_player_settings(const std::string& organization, const std::string& app, PlayerSettings& settings, std::string* error) {
    const std::string dir = user_data_dir(organization, app);
    if (dir.empty()) {
        if (error) *error = "no user data folder on this platform";
        return false;
    }
    return write_text_file(dir + "settings.json", player_settings_to_json(settings), error);
}

} // namespace rw
