#include "rewind/debug/overlay.h"
#include "rewind/core/player_settings.h"
#include "rewind/core/input_map.h"
#include "rewind/core/app.h"

#include "rewind/core/log.h"
#include "rewind/core/profiler.h"
#include "rewind/debug/inspector.h"
#include "rewind/net/session.h"
#include "rewind/sim/game.h"
#include "rewind/sim/reflect.h"
#include "rewind/sim/timeline.h"

#include "rewind/sim/savegame.h"
#include <filesystem>
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
static const ImVec4 kWarn(1.0f, 0.85f, 0.35f, 1);
static const ImVec4 kDim(0.6f, 0.6f, 0.65f, 1);

static ImVec4 mode_color(TimeMode mode) {
    switch (mode) {
    case TimeMode::Playing: return kGood;
    case TimeMode::Paused: return kWarn;
    case TimeMode::Rewinding: return ImVec4(0.55f, 0.75f, 1.0f, 1);
    case TimeMode::Replaying: return ImVec4(0.85f, 0.6f, 1.0f, 1);
    }
    return ImVec4(1, 1, 1, 1);
}

// ---- timeline -----------------------------------------------------------------------

void draw_timeline_panel(Timeline& tl, TimeControl& time, const FrameStats& stats) {
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
}

// ---- hot reload and network ----------------------------------------------------------

void draw_hot_reload_panel(const HotReloadInfo& hot) {
    if (hot.enabled) {
        ImGui::Text("hot reload: %s   build #%u", hot.module_name.c_str(), hot.generation);
        if (hot.generation > 1) {
            ImGui::TextColored(hot.history_reset ? kWarn : kGood,
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
}

void draw_network_panel(const net::SessionStats* net) {
    if (!net) return;
    const net::SessionStats& n = *net;
    if (n.desync) {
        ImGui::TextColored(kBad, "NETWORK DESYNC at tick %llu: the peers' states differ",
                           static_cast<unsigned long long>(n.desync_tick));
    } else if (!n.connected) {
        ImGui::TextColored(kWarn, "network: waiting for the peer");
    } else {
        ImGui::TextColored(kGood, "network: connected   rtt %.0f ms", n.rtt_ms);
    }
    ImGui::Text("rollbacks %u (%u ticks)   predicting %u ticks   stalls %u   pkts %u/%u", n.rollbacks,
                n.rolled_back_ticks, n.predicted_ticks, n.stalled_frames, n.packets_sent, n.packets_received);
}

// ---- profiler -------------------------------------------------------------------------

void draw_profiler_panel() {
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

// ---- console --------------------------------------------------------------------------

void draw_console_panel() {
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
    ImGui::BeginChild("##console", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (const LogEntry& e : entries) {
        const bool visible = (e.level == LogLevel::Debug && show_debug) || (e.level == LogLevel::Info && show_info) ||
                             (e.level == LogLevel::Warn && show_warn) || (e.level == LogLevel::Error && show_error);
        if (!visible) continue;
        if (filter[0] && e.text.find(filter) == std::string::npos && e.category.find(filter) == std::string::npos)
            continue;
        ImVec4 color = ImVec4(0.85f, 0.85f, 0.9f, 1);
        if (e.level == LogLevel::Debug) color = kDim;
        if (e.level == LogLevel::Warn) color = kWarn;
        if (e.level == LogLevel::Error) color = kBad;
        ImGui::TextColored(color, "%6.2fs #%-5u %-6s %s", static_cast<double>(e.time_ms) / 1000.0, e.frame,
                           e.category.c_str(), e.text.c_str());
    }
    if (autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// ---- state ----------------------------------------------------------------------------

void draw_state_panel(Timeline& tl, TimeControl& time, Game& game) {
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

void draw_keys_panel() {
    ImGui::BulletText("F1  toggle this overlay");
    ImGui::BulletText("F2  pause / resume (resuming from the past discards the old future)");
    ImGui::BulletText("F3  step one tick");
    ImGui::BulletText("F4  hold to rewind");
    ImGui::BulletText("F5  replay all history from the start and verify every tick");
    ImGui::BulletText("Esc quit");
    ImGui::Spacing();
    ImGui::TextDisabled("editor");
    ImGui::BulletText("Ctrl+Z / Ctrl+Y  undo / redo");
    ImGui::BulletText("Ctrl+S  save the scene (or the state without a project)");
    ImGui::BulletText("Ctrl+D  duplicate the selected entity, Del  delete it");
    ImGui::BulletText("scene view: wheel zooms (free camera), right/middle drag pans, right-click places");
    ImGui::BulletText("gizmos: Alt ignores snapping, Shift locks a drag to one axis");
}

void draw_settings_panel(App& app) {
    PlayerSettings& s = app.player_settings();
    bool changed = false;
    if (ImGui::Checkbox("fullscreen", &s.fullscreen)) changed = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("vsync", &s.vsync)) changed = true;
    ImGui::TextDisabled("volume");
    if (ImGui::SliderFloat("master", &s.audio.master, 0, 1)) changed = true;
    if (ImGui::SliderFloat("sfx", &s.audio.sfx, 0, 1)) changed = true;
    if (ImGui::SliderFloat("music", &s.audio.music, 0, 1)) changed = true;
    if (ImGui::SliderFloat("ui", &s.audio.ui, 0, 1)) changed = true;

    ImGui::TextDisabled("bindings");
    ImGui::SameLine();
    const bool custom = !s.input.empty();
    if (!custom) ImGui::TextDisabled("(project defaults; edit to make your own)");
    else if (ImGui::SmallButton("reset to project defaults")) {
        s.input = InputBindings{};
        changed = true;
    }

    // Capturing: which button waits for a key or pad press.
    static std::string capture_button;
    static bool capture_pad = false;
    const std::string pressed_key = app.take_pressed_key();
    const std::string pressed_pad = app.take_pressed_gamepad_button();
    if (!capture_button.empty()) {
        if (!capture_pad && !pressed_key.empty()) {
            if (pressed_key != "Escape") {
                if (s.input.empty()) s.input = app.active_bindings();
                s.input.add_key(capture_button, pressed_key);
                changed = true;
            }
            capture_button.clear();
        } else if (capture_pad && !pressed_pad.empty()) {
            if (s.input.empty()) s.input = app.active_bindings();
            s.input.add_gamepad(capture_button, pressed_pad);
            changed = true;
            capture_button.clear();
        }
    }

    const InputBindings& shown = custom ? s.input : app.active_bindings();
    if (ImGui::BeginTable("##bindings", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("button");
        ImGui::TableSetupColumn("keys");
        ImGui::TableSetupColumn("gamepad");
        ImGui::TableHeadersRow();
        for (u32 i = 0; i < kButtonCount; ++i) {
            const char* name = button_name(static_cast<Button>(1u << i));
            const ButtonBinding* b = shown.find(name);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name);
            ImGui::PushID(name);
            for (int col = 1; col <= 2; ++col) {
                ImGui::TableSetColumnIndex(col);
                ImGui::PushID(col);
                const bool pad = col == 2;
                if (b) {
                    const std::vector<std::string>& list = pad ? b->gamepad : b->keys;
                    for (usize k = 0; k < list.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        if (ImGui::SmallButton((list[k] + "  x").c_str())) {
                            if (s.input.empty()) s.input = app.active_bindings();
                            if (pad) s.input.remove_gamepad(name, list[k]);
                            else s.input.remove_key(name, list[k]);
                            changed = true;
                        }
                        ImGui::PopID();
                        ImGui::SameLine();
                    }
                }
                const bool waiting = capture_button == name && capture_pad == pad;
                if (waiting) {
                    ImGui::TextColored(ImVec4(1, 0.85f, 0.35f, 1), pad ? "press a pad button" : "press a key (Esc cancels)");
                } else if (ImGui::SmallButton("+")) {
                    capture_button = name;
                    capture_pad = pad;
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (changed) app.apply_player_settings();
    ImGui::Spacing();
    if (ImGui::Button("Save settings")) {
        std::string error;
        if (!app.save_player_settings(&error)) RW_LOG_ERROR("app", "settings: %s", error.c_str());
        else RW_LOG_INFO("app", "settings saved");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", user_data_dir(app.config().organization, app.config().title).c_str());
}

void draw_saves_panel(App& app) {
    static char slot[32] = "slot1";
    static char label[64] = "";
    ImGui::SetNextItemWidth(100);
    ImGui::InputText("slot", slot, sizeof slot);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##label", "label", label, sizeof label);
    ImGui::SameLine();
    std::string error;
    if (ImGui::Button("Save") && slot[0]) {
        if (!app.save_game(slot, label, &error)) RW_LOG_ERROR("app", "save: %s", error.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Load") && slot[0]) {
        if (!app.load_game(slot, &error)) RW_LOG_ERROR("app", "load: %s", error.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Screenshot (F12)")) app.screenshot();

    const std::vector<SaveInfo> saves = app.saved_games();
    if (saves.empty()) {
        ImGui::TextDisabled("no saves yet in %s", app.save_dir().c_str());
        return;
    }
    if (ImGui::BeginTable("##saves", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("slot");
        ImGui::TableSetupColumn("label");
        ImGui::TableSetupColumn("saved");
        ImGui::TableSetupColumn("tick");
        ImGui::TableHeadersRow();
        for (const SaveInfo& s : saves) {
            const std::string name = std::filesystem::path(s.file).stem().string();
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable((name + "##" + s.file).c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
                std::snprintf(slot, sizeof slot, "%s", name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (version %u, %zu bytes)", s.file.c_str(), s.version, s.bytes);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(s.label.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(format_timestamp(s.timestamp).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", static_cast<unsigned long long>(s.tick));
        }
        ImGui::EndTable();
    }
}

void apply_editor_theme(Color accent) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 4;
    s.FrameRounding = 3;
    s.GrabRounding = 3;
    s.TabRounding = 3;
    s.PopupRounding = 4;
    s.ScrollbarRounding = 6;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 0;
    s.TabBorderSize = 0;
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(6, 4);
    s.ItemSpacing = ImVec2(8, 5);
    s.IndentSpacing = 18;
    s.ScrollbarSize = 12;
    s.DockingSeparatorSize = 2;

    const ImVec4 a(accent.r / 255.0f, accent.g / 255.0f, accent.b / 255.0f, 1.0f);
    auto with_alpha = [&](float alpha) { return ImVec4(a.x, a.y, a.z, alpha); };
    auto grey = [](float v, float alpha = 1.0f) { return ImVec4(v, v, v, alpha); };
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = grey(0.90f);
    c[ImGuiCol_TextDisabled] = grey(0.52f);
    c[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.11f, 0.125f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.09f, 0.10f, 0.98f);
    c[ImGuiCol_Border] = grey(0.22f);
    c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.17f, 0.19f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.25f, 1.0f);
    c[ImGuiCol_FrameBgActive] = with_alpha(0.45f);
    c[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.09f, 0.10f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.09f, 0.09f, 0.10f, 1.0f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.09f, 0.09f, 0.10f, 1.0f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.09f, 0.09f, 0.10f, 0.6f);
    c[ImGuiCol_ScrollbarGrab] = grey(0.30f);
    c[ImGuiCol_ScrollbarGrabHovered] = grey(0.38f);
    c[ImGuiCol_ScrollbarGrabActive] = grey(0.46f);
    c[ImGuiCol_CheckMark] = a;
    c[ImGuiCol_SliderGrab] = with_alpha(0.85f);
    c[ImGuiCol_SliderGrabActive] = a;
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.23f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.27f, 0.31f, 1.0f);
    c[ImGuiCol_ButtonActive] = with_alpha(0.6f);
    c[ImGuiCol_Header] = with_alpha(0.28f);
    c[ImGuiCol_HeaderHovered] = with_alpha(0.42f);
    c[ImGuiCol_HeaderActive] = with_alpha(0.55f);
    c[ImGuiCol_Separator] = grey(0.22f);
    c[ImGuiCol_SeparatorHovered] = with_alpha(0.6f);
    c[ImGuiCol_SeparatorActive] = a;
    c[ImGuiCol_ResizeGrip] = grey(0.25f, 0.5f);
    c[ImGuiCol_ResizeGripHovered] = with_alpha(0.6f);
    c[ImGuiCol_ResizeGripActive] = a;
    c[ImGuiCol_Tab] = ImVec4(0.13f, 0.13f, 0.15f, 1.0f);
    c[ImGuiCol_TabHovered] = with_alpha(0.5f);
    c[ImGuiCol_TabSelected] = ImVec4(0.18f, 0.18f, 0.21f, 1.0f);
    c[ImGuiCol_TabSelectedOverline] = a;
    c[ImGuiCol_TabDimmed] = ImVec4(0.11f, 0.11f, 0.125f, 1.0f);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.15f, 0.15f, 0.17f, 1.0f);
    c[ImGuiCol_TabDimmedSelectedOverline] = with_alpha(0.5f);
    c[ImGuiCol_DockingPreview] = with_alpha(0.5f);
    c[ImGuiCol_DockingEmptyBg] = ImVec4(0.08f, 0.08f, 0.09f, 1.0f);
    c[ImGuiCol_PlotLines] = a;
    c[ImGuiCol_PlotHistogram] = a;
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.15f, 0.15f, 0.17f, 1.0f);
    c[ImGuiCol_TableBorderStrong] = grey(0.25f);
    c[ImGuiCol_TableBorderLight] = grey(0.18f);
    c[ImGuiCol_TableRowBgAlt] = grey(1.0f, 0.03f);
    c[ImGuiCol_TextSelectedBg] = with_alpha(0.35f);
    c[ImGuiCol_DragDropTarget] = a;
    c[ImGuiCol_NavCursor] = a;
}

// ---- the stacked overlay ------------------------------------------------------------

void draw_debug_overlay(Timeline& tl, TimeControl& time, Game& game, const FrameStats& stats,
                        const HotReloadInfo& hot, App* app) {
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Rewind")) {
        ImGui::End();
        return;
    }

    draw_timeline_panel(tl, time, stats);
    ImGui::Separator();
    draw_hot_reload_panel(hot);
    if (stats.net) {
        ImGui::Separator();
        draw_network_panel(stats.net);
    }

    if (ImGui::CollapsingHeader("Profiler")) draw_profiler_panel();

    {
        const u32 errors = log_count(LogLevel::Error);
        const u32 warnings = log_count(LogLevel::Warn) - errors;
        char title[64];
        std::snprintf(title, sizeof title, "Console (%u warnings, %u errors)###console", warnings, errors);
        if (errors > 0) ImGui::PushStyleColor(ImGuiCol_Text, kBad);
        else if (warnings > 0) ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
        const bool open = ImGui::CollapsingHeader(title);
        if (errors > 0 || warnings > 0) ImGui::PopStyleColor();
        if (open) {
            ImGui::BeginChild("##console_box", ImVec2(0, 180));
            draw_console_panel();
            ImGui::EndChild();
        }
    }

    if (ImGui::CollapsingHeader("Keys")) draw_keys_panel();

    if (app && ImGui::CollapsingHeader("Settings")) draw_settings_panel(*app);

    if (app && ImGui::CollapsingHeader("Saves")) draw_saves_panel(*app);

    if (ImGui::CollapsingHeader("Game", ImGuiTreeNodeFlags_DefaultOpen)) game.debug_ui();

    if (ImGui::CollapsingHeader("State")) draw_state_panel(tl, time, game);

    ImGui::End();
}

} // namespace rw
