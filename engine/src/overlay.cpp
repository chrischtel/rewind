#include "rewind/debug/overlay.h"

#include "rewind/debug/inspector.h"
#include "rewind/sim/game.h"
#include "rewind/sim/reflect.h"
#include "rewind/sim/timeline.h"

#include <imgui.h>

namespace rw {

const char* time_mode_name(TimeMode mode) {
    switch (mode) {
    case TimeMode::Playing: return "PLAYING";
    case TimeMode::Paused: return "PAUSED";
    case TimeMode::Rewinding: return "REWINDING";
    case TimeMode::Replaying: return "REPLAYING";
    }
    return "?";
}

static const ImVec4 kGood(0.45f, 0.95f, 0.55f, 1);
static const ImVec4 kBad(1, 0.35f, 0.35f, 1);
static const ImVec4 kDim(0.6f, 0.6f, 0.65f, 1);

static ImVec4 mode_color(TimeMode mode) {
    switch (mode) {
    case TimeMode::Playing: return ImVec4(0.45f, 0.95f, 0.55f, 1);
    case TimeMode::Paused: return ImVec4(1.0f, 0.85f, 0.35f, 1);
    case TimeMode::Rewinding: return ImVec4(0.55f, 0.75f, 1.0f, 1);
    case TimeMode::Replaying: return ImVec4(0.85f, 0.6f, 1.0f, 1);
    }
    return ImVec4(1, 1, 1, 1);
}

void draw_debug_overlay(Timeline& tl, TimeControl& time, Game& game, const FrameStats& stats,
                        const HotReloadInfo& hot) {
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Rewind")) {
        ImGui::End();
        return;
    }

    const TimelineConfig& cfg = tl.config();
    const Tick head = tl.head();
    const Tick oldest = tl.oldest();
    const Tick newest = tl.newest();
    const float seconds = static_cast<float>(newest - oldest) / static_cast<float>(cfg.tick_rate);

    ImGui::Text("%.0f fps   %.2f ms/frame   sim %.2f   render %.2f   voices %d", stats.fps,
                stats.frame_ms, stats.sim_ms, stats.render_ms, stats.audio_voices);
    ImGui::TextColored(kDim, "gpu %s   %d draw calls   %d vertices", stats.gpu_driver ? stats.gpu_driver : "?",
                       stats.draw_calls, stats.vertices);

    ImGui::Separator();
    ImGui::TextColored(mode_color(time.mode), "%s", time_mode_name(time.mode));
    ImGui::SameLine();
    ImGui::Text("  tick %llu / %llu   (%.1f s of history)", static_cast<unsigned long long>(head),
                static_cast<unsigned long long>(newest), seconds);
    ImGui::Text("state %016llx   %zu snapshots   %.2f MB",
                static_cast<unsigned long long>(tl.hash_at(head)), tl.keyframe_count(),
                static_cast<double>(tl.memory_bytes()) / (1024.0 * 1024.0));

    if (const auto& d = tl.last_desync()) {
        ImGui::TextColored(kBad, "DESYNC at tick %llu: expected %016llx, got %016llx",
                           static_cast<unsigned long long>(d->tick),
                           static_cast<unsigned long long>(d->expected),
                           static_cast<unsigned long long>(d->actual));
        ImGui::SameLine();
        if (ImGui::SmallButton("clear")) tl.clear_desync();
    } else {
        ImGui::TextColored(kGood, "deterministic: every re-simulated tick matched its fingerprint");
    }

    ImGui::Separator();

    // Transport controls.
    const bool playing = time.mode == TimeMode::Playing;
    if (ImGui::Button(playing ? "Pause (F2)" : "Play (F2)", ImVec2(100, 0))) {
        time.mode = playing ? TimeMode::Paused : TimeMode::Playing;
    }
    ImGui::SameLine();
    if (ImGui::Button("Step (F3)", ImVec2(90, 0))) {
        time.mode = TimeMode::Paused;
        time.step_request = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("< 1 s", ImVec2(60, 0))) {
        const Tick back = static_cast<Tick>(cfg.tick_rate);
        time.scrub_request = head > oldest + back ? head - back : oldest;
    }
    ImGui::SameLine();
    if (ImGui::Button("Replay all (F5)", ImVec2(120, 0))) time.replay_request = true;

    // Scrubber over the whole retained history.
    Tick scrub = head;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderScalar("##timeline", ImGuiDataType_U64, &scrub, &oldest, &newest, "tick %llu")) {
        time.scrub_request = scrub;
    }
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("rewind speed", &time.rewind_speed, 0.25f, 8.0f, "%.2fx");

    ImGui::Separator();
    if (hot.enabled) {
        ImGui::Text("hot reload: %s   build #%u", hot.module_name.c_str(), hot.generation);
        if (hot.generation > 1) {
            ImGui::TextColored(hot.history_reset ? ImVec4(1, 0.85f, 0.35f, 1) : kGood,
                               hot.history_reset
                                   ? "last reload changed the state layout: history was reset"
                                   : "last reload took over at tick %llu with history intact",
                               static_cast<unsigned long long>(hot.last_reload_tick));
        } else {
            ImGui::TextColored(kDim, "rebuild the game target while this runs to swap the code in");
        }
        if (!hot.last_error.empty()) ImGui::TextColored(kBad, "%s", hot.last_error.c_str());
    } else {
        ImGui::TextColored(kDim, "hot reload off: run this game through rewind_run to enable it");
    }

    if (ImGui::CollapsingHeader("Keys")) {
        ImGui::BulletText("F1  toggle this overlay");
        ImGui::BulletText("F2  pause / resume (resuming from the past discards the old future)");
        ImGui::BulletText("F3  step one tick");
        ImGui::BulletText("F4  hold to rewind");
        ImGui::BulletText("F5  replay all history from the start and verify every tick");
        ImGui::BulletText("Esc quit");
    }

    if (ImGui::CollapsingHeader("Game", ImGuiTreeNodeFlags_DefaultOpen)) {
        game.debug_ui();
    }

    if (ImGui::CollapsingHeader("State")) {
        ImGui::TextDisabled("every reflected field, editable while the game runs");
        ImGui::SameLine();
        if (ImGui::SmallButton("copy as text")) {
            DumpVisitor dump(32);
            game.reflect(dump);
            ImGui::SetClipboardText(dump.text().c_str());
        }
        ImGui::PushItemWidth(180);
        InspectorVisitor inspector;
        game.reflect(inspector);
        ImGui::PopItemWidth();
        if (inspector.changed()) {
            // The state no longer matches what the timeline recorded for this
            // tick: snapshot the edited state and make it the new truth.
            tl.recapture(game);
            if (time.mode == TimeMode::Replaying || time.mode == TimeMode::Rewinding) time.mode = TimeMode::Paused;
        }
    }

    ImGui::End();
}

} // namespace rw
