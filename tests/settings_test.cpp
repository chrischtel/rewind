// Headless checks for input bindings and player settings.

#include <rewind/core/input_map.h>
#include <rewind/core/player_settings.h>

#include <cstdio>
#include <string>

using namespace rw;

namespace {

int failures = 0;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

void test_bindings() {
    InputBindings b = InputBindings::defaults();
    CHECK(b.buttons.size() == kButtonCount);
    CHECK(b.find("a") && b.find("a")->keys.size() == 3);
    CHECK(!b.find("jump"));

    Button parsed;
    CHECK(button_from_name("start", parsed) && parsed == Button::Start);
    CHECK(!button_from_name("nope", parsed));
    CHECK(std::string(button_name(Button::Select)) == "select");

    // Names resolve through SDL, both ways.
    const int w = scancode_from_name("W");
    CHECK(w > 0 && key_name(w) == "W");
    CHECK(scancode_from_name("NoSuchKey") == -1);
    const int pad_a = gamepad_button_from_name("a");
    CHECK(pad_a >= 0 && gamepad_button_name(pad_a) == "a");

    CompiledBindings c = compile_bindings(b);
    CHECK(c.unknown.empty());
    CHECK((c.key_mask[static_cast<u32>(w)] & static_cast<u32>(Button::Up)) != 0);
    CHECK((c.key_mask[static_cast<u32>(scancode_from_name("Space"))] & static_cast<u32>(Button::A)) != 0);
    CHECK((c.pad_mask[static_cast<u32>(pad_a)] & static_cast<u32>(Button::A)) != 0);

    // Editing: add, no duplicates, remove; unknown names are reported.
    b.add_key("a", "F");
    b.add_key("a", "F");
    CHECK(b.find("a")->keys.size() == 4);
    b.remove_key("a", "Z");
    CHECK(b.find("a")->keys.size() == 3 && b.find("a")->keys[0] == "J");
    b.add_gamepad("jump", "a"); // unknown button name
    b.add_key("b", "Bogus");
    CompiledBindings d = compile_bindings(b);
    CHECK(d.unknown.size() == 2);

    // JSON round trip keeps names readable.
    const std::string json = to_json(b);
    CHECK(json.find("\"Space\"") != std::string::npos && json.find("\"dpup\"") != std::string::npos);
    InputBindings back;
    std::string error;
    CHECK(from_json(json, back, &error) && back.buttons.size() == b.buttons.size());
    CHECK(back.find("a")->keys == b.find("a")->keys);
}

void test_player_settings() {
    PlayerSettings s;
    s.fullscreen = true;
    s.audio.master = 0.5f;
    s.audio.music = 0.25f;
    s.input.add_key("a", "Space");
    JsonValue game;
    game.type = JsonValue::Type::Object;
    JsonValue level;
    level.type = JsonValue::Type::Number;
    level.text = "2";
    game.members.emplace_back("difficulty", level);
    s.game = game;
    const std::string text = player_settings_to_json(s);
    CHECK(text.find("difficulty") != std::string::npos);
    PlayerSettings back;
    std::string error;
    CHECK(player_settings_from_json(text, back, &error));
    CHECK(back.fullscreen && back.audio.master == 0.5f && back.audio.music == 0.25f);
    CHECK(back.input.find("a") && back.input.find("a")->keys[0] == "Space");
    CHECK(back.game.find("difficulty") && back.game.find("difficulty")->text == "2");
    CHECK(!player_settings_from_json("{ not json", back, &error) && !error.empty());

    // Round trip through the user folder.
    const std::string dir = user_data_dir("RewindTest", "settings_test");
    CHECK(!dir.empty());
    CHECK(save_player_settings("RewindTest", "settings_test", s, &error));
    PlayerSettings loaded;
    CHECK(load_player_settings("RewindTest", "settings_test", loaded, &error));
    CHECK(loaded.fullscreen && loaded.audio.master == 0.5f);
}

} // namespace

int main() {
    test_bindings();
    test_player_settings();
    if (failures == 0) std::printf("settings_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
