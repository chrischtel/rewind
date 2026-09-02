#pragma once

#include "rewind/audio/audio.h"
#include "rewind/core/file_watcher.h"
#include "rewind/core/types.h"
#include "rewind/debug/overlay.h"
#include "rewind/sim/events.h"
#include "rewind/sim/input.h"
#include "rewind/sim/timeline.h"

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

    TimelineConfig timeline{};
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

    Timeline& timeline() { return timeline_; }
    TimeControl& time_control() { return time_; }
    Renderer2D& renderer() { return *renderer_; }
    Audio& audio() { return audio_; }
    FileWatcher& assets() { return assets_; }

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
    InputFrame sample_input() const;

    AppConfig cfg_;
    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    Texture* target_ = nullptr;
    SDL_Gamepad* gamepad_ = nullptr;
    std::unique_ptr<Renderer2D> renderer_;
    Audio audio_;
    FileWatcher assets_;
    Timeline timeline_;
    TimeControl time_;
    FrameStats stats_;
    HotReloadInfo hot_;
    EventQueue events_;
    Game* game_ = nullptr;
    GameModule* module_ = nullptr;
    net::Session* session_ = nullptr;
    bool running_ = false;
    bool imgui_ready_ = false;
    double accumulator_ = 0;
};

} // namespace rw
