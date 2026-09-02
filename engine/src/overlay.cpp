#include "rewind/debug/overlay.h"

#include "rewind/core/log.h"
#include "rewind/core/profiler.h"
#include "rewind/debug/inspector.h"
#include "rewind/net/session.h"
#include "rewind/sim/game.h"
#include "rewind/sim/reflect.h"
#include "rewind/sim/timeline.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

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

static void draw_profiler() {
    const Profiler& p = Profiler::instance();

    static std::vector<float> history;
    p.history(history);
    char overlay_text[64];
    std::snprintf(overlay_text, sizeof overlay_text, "avg %.2f ms   worst %.2f ms", p.average_ms(), p.worst_ms());
    if (!history.empty()) {
        ImGui::PlotLines("##frames", history.data(), static_cast<int>(history.size()), 0, overlay_text, 0.0f,
                         std::max(20.0f, p.worst_ms()), ImVec2(-1, 60));
    }

    // Timeline bars: one row per nesting depth, scaled to the frame length.
    const std::vector<ProfileSample>& samples = p.last_frame();
    const float frame_ns = static_cast<float>(std::max<u64>(p.last_frame_ns(), 1));
    int max_depth = 0;
    for (const ProfileSample& s : samples) max_depth = std::max(max_depth, s.depth);

    const float width = ImGui::GetContentRegionAvail().x;
    const float row_h = 14.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 colors[] = {IM_COL32(90, 160, 255, 255), IM_COL32(90, 220, 140, 255), IM_COL32(255, 190, 80, 255),
                            IM_COL32(230, 110, 110, 255), IM_COL32(190, 130, 255, 255)};
    for (const ProfileSample& s : samples) {
        const float x0 = origin.x + width * static_cast<float>(s.begin_ns) / frame_ns;
        const float x1 = origin.x + width * static_cast<float>(s.end_ns) / frame_ns;
        const float y0 = origin.y + row_h * static_cast<float>(s.depth);
        const ImU32 color = colors[static_cast<usize>(s.depth) % 5];
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(std::max(x1, x0 + 1.0f), y0 + row_h - 2), color);
        if (x1 - x0 > 40) dl->AddText(ImVec2(x0 + 3, y0), IM_COL32(0, 0, 0, 255), s.name);
        if (ImGui::IsMouseHoveringRect(ImVec2(x0, y0), ImVec2(x1, y0 + row_h))) {
            ImGui::SetTooltip("%s: %.3f ms", s.name, static_cast<float>(s.duration_ns()) / 1e6f);
        }
    }
    ImGui::Dummy(ImVec2(width, row_h * static_cast<float>(max_depth + 1) + 4));

    // Totals per name for the last frame.
    struct Total {
        const char* name;
        u64 ns;
        int calls;
    };
    std::vector<Total> totals;
    for (const ProfileSample& s : samples) {
        auto it = std::find_if(totals.begin(), totals.end(), [&](const Total& t) { return std::strcmp(t.name, s.name) == 0; });
        if (it == totals.end()) totals.push_back(Total{s.name, s.duration_ns(), 1});
        else {
            it->ns += s.duration_ns();
            ++it->calls;
        }
    }
    if (ImGui::BeginTable("##totals", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("region");
        ImGui::TableSetupColumn("ms");
        ImGui::TableSetupColumn("calls");
        ImGui::TableHeadersRow();
        for (const Total& t : totals) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(t.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", static_cast<float>(t.ns) / 1e6f);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", t.calls);
        }
        ImGui::EndTable();
    }
}

static void draw_console() {
    static char filter[64] = "";
    static bool show_debug = false, show_info = true, show_warn = true, show_error = true;
    static bool autoscroll = true;
    static std::vector<LogEntry> entries;

    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##filter", "filter text or category", filter, sizeof filter);
    ImGui::SameLine();
    ImGui::Checkbox("debug", &show_debug);
    ImGui::SameLine();
    ImGui::Checkbox("info", &show_info);
    ImGui::SameLine();
    ImGui::Checkbox("warn", &show_warn);
    ImGui::SameLine();
    ImGui::Checkbox("error", &show_error);
    ImGui::SameLine();
    ImGui::Checkbox("follow", &autoscroll);
    ImGui::SameLine();
    if (ImGui::SmallButton("clear")) log_clear();

    log_snapshot(entries);
    ImGui::BeginChild("##console", ImVec2(0, 160), ImGuiChildFlags_Borders);
    for (const LogEntry& e : entries) {
        const bool visible = (e.level == LogLevel::Debug && show_debug) || (e.level == LogLevel::Info && show_info) ||
                             (e.level == LogLevel::Warn && show_warn) || (e.level == LogLevel::Error && show_error);
        if (!visible) continue;
        if (filter[0] && e.text.find(filter) == std::string::npos && e.category.find(filter) == std::string::npos)
            continue;
        ImVec4 color = ImVec4(0.85f, 0.85f, 0.9f, 1);
        if (e.level == LogLevel::Debug) color = kDim;
        if (e.level == LogLevel::Warn) color = ImVec4(1.0f, 0.85f, 0.35f, 1);
        if (e.level == LogLevel::Error) color = kBad;
        ImGui::TextColored(color, "%6.2fs #%-5u %-6s %s", static_cast<double>(e.time_ms) / 1000.0, e.frame,
                           e.category.c_str(), e.text.c_str());
    }
    if (autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
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
    if (hot.asset_reloads > 0) {
        ImGui::TextColored(kGood, "assets: %zu files watched, %u reloads, last %s", hot.tracked_files,
                           hot.asset_reloads, hot.last_asset.c_str());
    } else {
        ImGui::TextColored(kDim, "assets: %zu files watched, edits reload live", hot.tracked_files);
    }

    if (stats.net) {
        const net::SessionStats& n = *stats.net;
        ImGui::Separator();
        if (n.desync) {
            ImGui::TextColored(kBad, "NETWORK DESYNC at tick %llu: the peers' states differ",
                               static_cast<unsigned long long>(n.desync_tick));
        } else if (!n.connected) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1), "network: waiting for the peer");
        } else {
            ImGui::TextColored(kGood, "network: connected   rtt %.0f ms", n.rtt_ms);
        }
        ImGui::Text("rollbacks %u (%u ticks)   predicting %u ticks   stalls %u   pkts %u/%u", n.rollbacks,
                    n.rolled_back_ticks, n.predicted_ticks, n.stalled_frames, n.packets_sent, n.packets_received);
    }

    if (ImGui::CollapsingHeader("Profiler")) {
        draw_profiler();
    }

    {
        const u32 errors = log_count(LogLevel::Error);
        const u32 warnings = log_count(LogLevel::Warn) - errors;
        char title[64];
        std::snprintf(title, sizeof title, "Console (%u warnings, %u errors)###console", warnings, errors);
        if (errors > 0) ImGui::PushStyleColor(ImGuiCol_Text, kBad);
        else if (warnings > 0) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.35f, 1));
        const bool open = ImGui::CollapsingHeader(title);
        if (errors > 0 || warnings > 0) ImGui::PopStyleColor();
        if (open) draw_console();
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
