#pragma once

#include "rewind/audio/audio.h"
#include "rewind/core/input_map.h"
#include "rewind/core/player_settings.h"
#include "rewind/sim/savegame.h"
#include "rewind/core/file_watcher.h"
#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/debug/overlay.h"
#include "rewind/sim/events.h"
#include "rewind/sim/input.h"
#include "rewind/sim/reflect.h"
#include "rewind/sim/timeline.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_Gamepad;

namespace rw {

class Game;
class GameModule;
class Renderer2D;
struct Texture;
namespace net {
class Session;
}

struct AppConfig {
    std::string title = "Rewind";
    std::vector<std::string> args; ///< Command line, without the program name.
    int width = 1280;
    int height = 720;
    bool vsync = true;
    bool resizable = true;
    bool start_with_overlay = true;
    bool audio = true; ///< Open the default playback device. Off = play() is a no-op.

    /// GPU backend: "" lets SDL choose (Direct3D 12 or Vulkan on Windows),
    /// or force "vulkan" / "direct3d12" / "metal".
    std::string gpu_driver;
    bool gpu_debug = false; ///< Validation layers on. Slower, very talkative, very useful.

    /// Pixel mode: draw the game into a small render target of this size and
    /// scale it up to the window. 0 means draw at window resolution.
    int render_width = 0;
    int render_height = 0;
    bool integer_scale = true; ///< Pixel mode only: whole-number scale, letterbox the rest.
    bool pixel_art = true;     ///< Nearest-neighbour sampling for textures and the target.

    /// Embedded mode, used by the editor: the game always renders into a
    /// texture that the host shows wherever it likes (see App::view_texture),
    /// the built-in overlay stays off, and the host draws its own UI through
    /// the callback set with App::set_ui_callback().
    bool embedded = false;

    /// TrueType file for the debug overlay and editor UI; empty keeps ImGui's
    /// built-in font. Sized in pixels.
    std::string ui_font;
    float ui_font_size = 16.0f;

    TimelineConfig timeline{};

    /// Organisation name for the per-user settings folder (SDL_GetPrefPath).
    std::string organization = "Rewind";
    bool fullscreen = false; ///< Borderless fullscreen on the window's display.
    /// Bump when the reflected state changes shape; older saves go through Game::migrate_save().
    u32 save_version = 1;
    /// Keys and gamepad buttons per abstract Button; empty = InputBindings::defaults().
    InputBindings input;
    AudioMix audio_mix;

    /// Project settings: everything a project file may override.
    void reflect(Visitor& v) {
        v.field("title", title);
        v.field("organization", organization, {.tooltip = "folder name for the per-user settings"});
        v.field("save_version", save_version, {.tooltip = "bump when the saved state changes shape"});
        v.field("fullscreen", fullscreen);
        v.field("width", width, {.min = 320, .max = 7680});
        v.field("height", height, {.min = 200, .max = 4320});
        v.field("vsync", vsync);
        v.field("resizable", resizable);
        v.field("start_with_overlay", start_with_overlay);
        v.field("audio", audio);
        v.field("gpu_driver", gpu_driver, {.tooltip = "empty = automatic, or vulkan / direct3d12 / metal"});
        v.field("gpu_debug", gpu_debug);
        v.field("render_width", render_width, {.min = 0, .max = 4096, .tooltip = "0 = window resolution"});
        v.field("render_height", render_height, {.min = 0, .max = 4096});
        v.field("integer_scale", integer_scale);
        v.field("pixel_art", pixel_art);
        v.field("ui_font", ui_font, {.tooltip = "TrueType file for the overlay and editor, empty = built-in"});
        v.field("ui_font_size", ui_font_size, {.min = 8, .max = 40});
        v.field("timeline", timeline);
        v.field("input", input);
        v.field("audio_mix", audio_mix);
    }
};

/// Owns the window, the GPU device, the fixed-step loop, the timeline, audio,
/// and the debug overlay.
///
/// Hotkeys while running:
///   F1  toggle overlay        F4  hold to rewind
///   F2  pause / resume        F5  replay all history and verify determinism
///   F3  step one tick         Esc quit
class App {
public:
    using UiCallback = std::function<void(App&)>;

    explicit App(AppConfig cfg);
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// Run a game that is linked into this executable. Returns the exit code.
    int run(Game& game);

    /// Run a game from a DLL and reload it whenever the DLL is rebuilt.
    /// The module must already be loaded. Returns the exit code.
    int run(GameModule& module);

    void quit() { running_ = false; }

    /// Drive the simulation through a rollback session instead of directly.
    /// The session must outlive the App loop; pass nullptr to detach.
    void set_session(net::Session* session) { session_ = session; }
    net::Session* session() const { return session_; }

    // ---- embedded mode (editor) ---------------------------------------------
    /// Called every frame between ImGui::NewFrame() and ImGui::Render().
    void set_ui_callback(UiCallback callback) { ui_ = std::move(callback); }

    /// Called once after Game::init() and before the timeline records tick 0:
    /// the place for a host to load a startup scene into the game's world.
    void set_start_callback(UiCallback callback) { start_ = std::move(callback); }

    /// Called right after Game::render() with the same target and camera
    /// still set: tools draw gizmos and guides into the game's view here.
    using SceneCallback = std::function<void(App&, Renderer2D&)>;
    void set_scene_callback(SceneCallback callback) { scene_ = std::move(callback); }

    /// The texture the game was drawn into this frame, sized by set_view_size().
    Texture* view_texture() const { return view_; }

    /// Ask for the view texture to be this size; the game's render target is
    /// scaled into it with the configured filtering (integer steps in pixel mode).
    void set_view_size(int width, int height);

    /// Where the view texture is shown on screen, so mouse input maps into
    /// the game; and whether the game should receive keyboard and mouse
    /// input at all right now (the host decides by focus).
    void set_view_rect(const Rect& rect);
    void set_game_input_focus(bool focused) { input_focus_ = focused; }
    bool game_input_focus() const { return input_focus_; }

    // ---- access ---------------------------------------------------------------
    Timeline& timeline() { return timeline_; }
    TimeControl& time_control() { return time_; }
    Renderer2D& renderer() { return *renderer_; }
    Audio& audio() { return audio_; }
    FileWatcher& assets() { return assets_; }
    Game& game() { return *game_; }
    const AppConfig& config() const { return cfg_; }
    const FrameStats& stats() const { return stats_; }
    const HotReloadInfo& hot_reload() const { return hot_; }
    /// Load an entire simulation state (as produced by Game::save) and make
    /// it the current tick, discarding the future. Returns false if the
    /// bytes did not fit the game's layout.
    bool load_state(const std::vector<u8>& bytes);

    // ---- player settings ----------------------------------------------------
    /// The per-user options, loaded at start (fullscreen, vsync, volumes,
    /// bindings, the game's own block). Edit, then apply_player_settings().
    PlayerSettings& player_settings() { return settings_; }
    /// Push the settings into the window, the audio mix and the bindings.
    void apply_player_settings();
    /// Write settings.json in the user's folder for this organisation and title.
    bool save_player_settings(std::string* error = nullptr);
    void set_fullscreen(bool on);
    bool fullscreen() const;
    /// The bindings in effect: the player's, else the project's, else the defaults.
    const InputBindings& active_bindings() const { return active_bindings_; }
    const CompiledBindings& compiled_bindings() const { return compiled_; }
    /// For rebinding UIs: the key or gamepad button pressed since the last
    /// call, as a name, or empty. Consumed on read.
    std::string take_pressed_key();
    std::string take_pressed_gamepad_button();

    // ---- save games ---------------------------------------------------------
    /// Write the state to "<saves dir>/<slot>.rwsave" with the config's
    /// save_version; load_game reads it back (migrating through
    /// Game::migrate_save when the version differs) and makes it the current
    /// tick. Slots are file names: "slot1", "auto", "quick".
    bool save_game(const std::string& slot, const std::string& label = "", std::string* error = nullptr);
    bool load_game(const std::string& slot, std::string* error = nullptr);
    std::vector<SaveInfo> saved_games() const;
    std::string save_dir() const;

    // ---- screenshots --------------------------------------------------------
    /// Capture the next frame (game only, no overlay) to
    /// "<user data>/screenshots/<timestamp>.png". F12 does the same.
    void screenshot(const std::string& file = "");

private:
    int loop();
    bool init_platform();
    void shutdown_platform();
    void handle_events();
    void poll_hot_reload();
    void poll_assets();
    void apply_requests(const InputFrame& live);
    void simulate(const InputFrame& live, double frame_dt);
    void dispatch_events();
    void render_frame(float alpha);
    void present_target();
    void present_to_view();
    InputFrame sample_input() const;

    AppConfig cfg_;
    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    Texture* target_ = nullptr;
    Texture* view_ = nullptr;
    int view_w_ = 0, view_h_ = 0;
    SDL_Gamepad* gamepad_ = nullptr;
    std::unique_ptr<Renderer2D> renderer_;
    Audio audio_;
    FileWatcher assets_;
    Timeline timeline_;
    TimeControl time_;
    FrameStats stats_;
    HotReloadInfo hot_;
    EventQueue events_;
    UiCallback ui_;
    SceneCallback scene_;
    UiCallback start_;
    Game* game_ = nullptr;
    GameModule* module_ = nullptr;
    net::Session* session_ = nullptr;
    bool running_ = false;
    bool imgui_ready_ = false;
    bool input_focus_ = true;
    mutable i32 wheel_accum_ = 0; ///< wheel clicks since sample_input() last took them
    PlayerSettings settings_;
    InputBindings active_bindings_;
    CompiledBindings compiled_;
    std::string pressed_key_;
    std::string pressed_pad_;
    bool screenshot_pending_ = false;
    std::string screenshot_file_;
    Texture* screenshot_target_ = nullptr;
    double accumulator_ = 0;
};

} // namespace rw
