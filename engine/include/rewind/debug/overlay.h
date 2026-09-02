#pragma once

#include "rewind/core/types.h"

#include <optional>
#include <string>

namespace rw {

class Game;
class Timeline;
namespace net {
struct SessionStats;
}

enum class TimeMode {
    Playing,   ///< Normal: simulate with live input.
    Paused,    ///< Frozen. Step, scrub, or resume.
    Rewinding, ///< Moving backwards through history in real time.
    Replaying, ///< Re-simulating recorded input from the current head to the end.
};

/// Debug time controls. The overlay and hotkeys write requests here; the App
/// applies them at a safe point in the frame.
struct TimeControl {
    TimeMode mode = TimeMode::Playing;
    float rewind_speed = 1.0f;
    bool show_overlay = true;

    std::optional<Tick> scrub_request;
    bool step_request = false;
    bool replay_request = false;
};

struct FrameStats {
    float fps = 0;
    float frame_ms = 0;
    float sim_ms = 0;
    float render_ms = 0;
    int audio_voices = 0;
    int draw_calls = 0;
    int vertices = 0;
    const char* gpu_driver = "";
    const net::SessionStats* net = nullptr; ///< Set while a rollback session drives the game.
};

/// What the overlay shows about hot reload. Filled in by App when the game
/// comes from a GameModule.
struct HotReloadInfo {
    bool enabled = false;
    std::string module_name;
    u32 generation = 0;
    Tick last_reload_tick = 0;
    bool history_reset = false;
    std::string last_error;

    // Asset hot reload (always on).
    usize tracked_files = 0;
    u32 asset_reloads = 0;
    std::string last_asset;
};

const char* time_mode_name(TimeMode mode);

/// Draws the engine's Dear ImGui overlay: stats, timeline scrubber, determinism
/// status, hot reload status, and the game's own debug_ui(). Must be called
/// between ImGui::NewFrame() and ImGui::Render().
void draw_debug_overlay(Timeline& timeline, TimeControl& time, Game& game, const FrameStats& stats,
                        const HotReloadInfo& hot);

} // namespace rw
