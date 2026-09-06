#include "rewind/core/app.h"

#include "rewind/core/input_map.h"
#include "rewind/core/player_settings.h"
#include "rewind/assets/png.h"
#include "rewind/sim/savegame.h"
#include "rewind/core/log.h"

#include "rewind/core/game_module.h"
#include "rewind/core/paths.h"
#include "rewind/core/profiler.h"
#include "rewind/net/session.h"
#include "rewind/render/renderer2d.h"
#include "rewind/sim/game.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>

namespace rw {

App::App(AppConfig cfg) : cfg_(std::move(cfg)), timeline_(cfg_.timeline) {
    time_.show_overlay = cfg_.start_with_overlay;
}

App::~App() { shutdown_platform(); }

bool App::init_platform() {
    log_capture_sdl();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        RW_LOG_ERROR("app", "SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    SDL_WindowFlags flags = 0;
    if (cfg_.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (cfg_.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    window_ = SDL_CreateWindow(cfg_.title.c_str(), cfg_.width, cfg_.height, flags);
    if (!window_) {
        RW_LOG_ERROR("app", "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    // Shader formats we can actually supply: SPIR-V when it was embedded at
    // build time, DXBC compiled at runtime on Windows.
    SDL_GPUShaderFormat formats = 0;
#if defined(_WIN32)
    formats |= SDL_GPU_SHADERFORMAT_DXBC;
#endif
    formats |= SDL_GPU_SHADERFORMAT_SPIRV;
    device_ = SDL_CreateGPUDevice(formats, cfg_.gpu_debug, cfg_.gpu_driver.empty() ? nullptr : cfg_.gpu_driver.c_str());
    if (!device_) {
        RW_LOG_ERROR("app", "SDL_CreateGPUDevice failed: %s", SDL_GetError());
        return false;
    }
    if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
        RW_LOG_ERROR("app", "SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        return false;
    }
    SDL_GPUPresentMode present = SDL_GPU_PRESENTMODE_VSYNC;
    if (!cfg_.vsync) {
        if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_MAILBOX))
            present = SDL_GPU_PRESENTMODE_MAILBOX;
        else if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_IMMEDIATE))
            present = SDL_GPU_PRESENTMODE_IMMEDIATE;
    }
    SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, present);
    RW_LOG_INFO("app", "gpu: %s", SDL_GetGPUDeviceDriver(device_));

    renderer_ = std::make_unique<Renderer2D>(device_, window_);
    if (!renderer_->ok()) {
        RW_LOG_ERROR("app", "the renderer could not be set up");
        return false;
    }
    renderer_->set_pixel_art(cfg_.pixel_art);
    renderer_->set_file_watcher(&assets_);
    audio_.set_file_watcher(&assets_);

    if (cfg_.embedded && cfg_.render_width <= 0) {
        // The host shows the game in a panel: render at the configured window size.
        cfg_.render_width = cfg_.width;
        cfg_.render_height = cfg_.height;
        cfg_.integer_scale = false;
    }
    if (cfg_.render_width > 0 && cfg_.render_height > 0) {
        target_ = renderer_->create_render_target("__pixel_target", cfg_.render_width, cfg_.render_height);
        if (!target_) {
            RW_LOG_ERROR("app", "could not create the %dx%d render target", cfg_.render_width, cfg_.render_height);
            return false;
        }
        RW_LOG_INFO("app", "pixel mode: %dx%d, %s scaling", cfg_.render_width, cfg_.render_height,
                cfg_.integer_scale ? "integer" : "fractional");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // do not litter the working directory
    if (cfg_.embedded) {
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        time_.show_overlay = false;
    }
    if (!cfg_.ui_font.empty()) {
        const std::string file = resolve_asset_path(cfg_.ui_font);
        if (!io.Fonts->AddFontFromFileTTF(file.c_str(), cfg_.ui_font_size))
            RW_LOG_WARN("app", "ui font %s could not be loaded, using the built-in one", cfg_.ui_font.c_str());
    }
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLGPU(window_);
    ImGui_ImplSDLGPU3_InitInfo init{};
    init.Device = device_;
    init.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    init.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init);
    imgui_ready_ = true;

    if (cfg_.audio) audio_.init(); // failure is logged and leaves audio silent

    // Per-user options over the project's settings; missing file = defaults.
    settings_.fullscreen = cfg_.fullscreen;
    settings_.vsync = cfg_.vsync;
    settings_.audio = cfg_.audio_mix;
    std::string settings_error;
    if (load_player_settings(cfg_.organization, cfg_.title, settings_, &settings_error))
        RW_LOG_INFO("app", "player settings loaded from %s", user_data_dir(cfg_.organization, cfg_.title).c_str());
    apply_player_settings();
    return true;
}

void App::shutdown_platform() {
    if (!window_) return;
    audio_.shutdown();
    if (device_) SDL_WaitForGPUIdle(device_);
    if (imgui_ready_) {
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        imgui_ready_ = false;
    }
    target_ = nullptr;
    renderer_.reset();
    if (gamepad_) SDL_CloseGamepad(gamepad_);
    gamepad_ = nullptr;
    if (device_) {
        SDL_ReleaseWindowFromGPUDevice(device_, window_);
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
    }
    SDL_DestroyWindow(window_);
    SDL_Quit();
    window_ = nullptr;
}

void App::handle_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        ImGui_ImplSDL3_ProcessEvent(&e);
        switch (e.type) {
        case SDL_EVENT_QUIT: running_ = false; break;

        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break;
            pressed_key_ = key_name(e.key.scancode);
            switch (e.key.scancode) {
            case SDL_SCANCODE_ESCAPE: running_ = false; break;
            case SDL_SCANCODE_F1: time_.show_overlay = !time_.show_overlay; break;
            case SDL_SCANCODE_F2:
                time_.mode = time_.mode == TimeMode::Playing ? TimeMode::Paused : TimeMode::Playing;
                break;
            case SDL_SCANCODE_F3:
                time_.mode = TimeMode::Paused;
                time_.step_request = true;
                break;
            case SDL_SCANCODE_F4: time_.mode = TimeMode::Rewinding; break;
            case SDL_SCANCODE_F5: time_.replay_request = true; break;
            case SDL_SCANCODE_F12: screenshot(); break;
            default: break;
            }
            break;

        case SDL_EVENT_KEY_UP:
            if (e.key.scancode == SDL_SCANCODE_F4 && time_.mode == TimeMode::Rewinding) {
                time_.mode = TimeMode::Paused;
            }
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            wheel_accum_ += static_cast<i32>(e.wheel.y > 0 ? std::ceil(e.wheel.y) : std::floor(e.wheel.y));
            break;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            pressed_pad_ = gamepad_button_name(e.gbutton.button);
            break;

        case SDL_EVENT_GAMEPAD_ADDED:
            if (!gamepad_) {
                gamepad_ = SDL_OpenGamepad(e.gdevice.which);
                if (gamepad_) RW_LOG_INFO("app", "gamepad connected: %s", SDL_GetGamepadName(gamepad_));
            }
            break;

        case SDL_EVENT_GAMEPAD_REMOVED:
            if (gamepad_ && SDL_GetGamepadID(gamepad_) == e.gdevice.which) {
                SDL_CloseGamepad(gamepad_);
                gamepad_ = nullptr;
                RW_LOG_INFO("app", "gamepad disconnected");
            }
            break;

        default: break;
        }
    }
}

InputFrame App::sample_input() const {
    InputFrame in;
    const bool keyboard = cfg_.embedded ? input_focus_ : !ImGui::GetIO().WantCaptureKeyboard;

    // Bindings: every held key or pad button contributes its Button bits.
    u32 buttons = 0;
    if (keyboard) {
        int count = 0;
        const bool* k = SDL_GetKeyboardState(&count);
        const int n = std::min(count, static_cast<int>(CompiledBindings::kScancodes));
        for (int s = 0; s < n; ++s)
            if (k[s]) buttons |= compiled_.key_mask[s];
    }

    i32 axis_x = 0;
    i32 axis_y = 0;

    if (gamepad_) {
        for (u32 b = 0; b < CompiledBindings::kPadButtons && b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
            if (compiled_.pad_mask[b] && SDL_GetGamepadButton(gamepad_, static_cast<SDL_GamepadButton>(b)))
                buttons |= compiled_.pad_mask[b];

        const i32 deadzone = static_cast<i32>(std::clamp(compiled_.deadzone, 0.0f, 0.9f) * 32767.0f);
        const i32 sx = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX);
        const i32 sy = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTY);
        if (std::abs(sx) > deadzone) axis_x = std::clamp(sx, -32767, 32767);
        if (std::abs(sy) > deadzone) axis_y = std::clamp(sy, -32767, 32767);
    }
    in.buttons = buttons;

    // Digital directions always win over the stick so keyboard and d-pad feel exact.
    const int dx = (in.down(Button::Right) ? 1 : 0) - (in.down(Button::Left) ? 1 : 0);
    const int dy = (in.down(Button::Down) ? 1 : 0) - (in.down(Button::Up) ? 1 : 0);
    if (dx != 0) axis_x = dx * 32767;
    if (dy != 0) axis_y = dy * 32767;

    in.axis_x = static_cast<i16>(axis_x);
    in.axis_y = static_cast<i16>(axis_y);

    // Mouse, mapped into render-target pixels so games in pixel mode see their own coordinates.
    float mx = 0, my = 0;
    const SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
    const Vec2 rp = renderer_->window_to_render(mx, my);
    in.mouse_x = static_cast<i16>(std::clamp(rp.x, -32000.0f, 32000.0f));
    in.mouse_y = static_cast<i16>(std::clamp(rp.y, -32000.0f, 32000.0f));
    const bool mouse = cfg_.embedded ? input_focus_ : !ImGui::GetIO().WantCaptureMouse;
    if (mouse) {
        in.set(Button::MouseLeft, (mb & SDL_BUTTON_LMASK) != 0);
        in.set(Button::MouseRight, (mb & SDL_BUTTON_RMASK) != 0);
        in.set(Button::MouseMiddle, (mb & SDL_BUTTON_MMASK) != 0);
        in.wheel = static_cast<i8>(std::clamp(wheel_accum_, -127, 127));
    }
    wheel_accum_ = 0;
    return in;
}

void App::poll_hot_reload() {
    if (!module_) return;
    const GameModule::Reload reload = module_->poll();

    switch (reload.status) {
    case GameModule::Reload::Status::NoChange: break;

    case GameModule::Reload::Status::Failed:
        if (hot_.last_error != reload.error) {
            RW_LOG_ERROR("app", "hot reload failed: %s", reload.error.c_str());
        }
        hot_.last_error = reload.error;
        break;

    case GameModule::Reload::Status::Reloaded:
        game_ = module_->game();
        hot_.generation = module_->generation();
        hot_.last_reload_tick = timeline_.head();
        hot_.last_error.clear();
        hot_.history_reset = !reload.state_compatible;
        game_->load_assets(*renderer_, audio_);
        if (reload.state_compatible) {
            RW_LOG_INFO("app", "hot reload #%u: new code took over at tick %llu, history kept", hot_.generation,
                    static_cast<unsigned long long>(timeline_.head()));
        } else {
            // The new build's state layout differs from what was recorded, so
            // every snapshot in the timeline is unreadable to it. Start over.
            game_->init();
            timeline_.reset(*game_);
            accumulator_ = 0;
            RW_LOG_INFO("app", "hot reload #%u: state layout changed, started a fresh timeline",
                    hot_.generation);
        }
        break;
    }
}

void App::poll_assets() {
    const std::vector<std::string> changed = assets_.poll();
    if (changed.empty()) return;
    for (const std::string& path : changed) {
        renderer_->invalidate_file(path);
        audio_.invalidate_file(path);
        RW_LOG_INFO("app", "asset changed: %s", path.c_str());
    }
    RW_PROFILE_SCOPE("load_assets");
    game_->load_assets(*renderer_, audio_);
    hot_.asset_reloads += static_cast<u32>(changed.size());
    hot_.last_asset = changed.back();
}

void App::apply_requests(const InputFrame& live) {
    if (time_.scrub_request) {
        timeline_.seek(*game_, *time_.scrub_request);
        time_.scrub_request.reset();
        time_.mode = TimeMode::Paused;
        accumulator_ = 0;
    }
    if (time_.step_request) {
        time_.step_request = false;
        timeline_.step_forward(*game_, live);
        accumulator_ = 0;
    }
    if (time_.replay_request) {
        time_.replay_request = false;
        timeline_.clear_desync();
        timeline_.seek(*game_, timeline_.oldest());
        time_.mode = TimeMode::Replaying;
        accumulator_ = 0;
    }
}

void App::simulate(const InputFrame& live, double frame_dt) {
    const double dt = timeline_.dt();
    switch (time_.mode) {
    case TimeMode::Playing:
        accumulator_ += frame_dt;
        if (session_) {
            // The session decides how far we may run; stalls leave time in
            // the accumulator, capped so a long stall does not turn into a sprint.
            const u32 wanted = static_cast<u32>(accumulator_ / dt);
            const u32 advanced = session_->step(timeline_, *game_, live, wanted, &events_, SDL_GetTicks());
            accumulator_ -= advanced * dt;
            accumulator_ = std::min(accumulator_, dt * 2.0);
            break;
        }
        while (accumulator_ >= dt) {
            timeline_.advance(*game_, live, &events_);
            accumulator_ -= dt;
        }
        break;

    case TimeMode::Rewinding:
        accumulator_ += frame_dt * time_.rewind_speed;
        while (accumulator_ >= dt) {
            accumulator_ -= dt;
            if (timeline_.head() == timeline_.oldest()) {
                time_.mode = TimeMode::Paused;
                accumulator_ = 0;
                break;
            }
            timeline_.seek(*game_, timeline_.head() - 1);
        }
        break;

    case TimeMode::Replaying:
        accumulator_ += frame_dt;
        while (accumulator_ >= dt) {
            accumulator_ -= dt;
            if (timeline_.at_end()) {
                time_.mode = TimeMode::Paused;
                accumulator_ = 0;
                break;
            }
            timeline_.resimulate(*game_, timeline_.head() + 1);
        }
        break;

    case TimeMode::Paused: accumulator_ = 0; break;
    }
}

void App::dispatch_events() {
    // Only ticks that ran live put anything here; seeks and replays emit into nothing.
    for (const SimEvent& e : events_.items) game_->on_event(e, audio_);
    events_.clear();
}

void App::present_target() {
    // Scale the low-res target up into the window, centred, black bars around it.
    const float win_w = static_cast<float>(renderer_->window_width());
    const float win_h = static_cast<float>(renderer_->window_height());
    const float rw = static_cast<float>(cfg_.render_width);
    const float rh = static_cast<float>(cfg_.render_height);
    if (win_w <= 0 || win_h <= 0) return;

    float scale = std::min(win_w / rw, win_h / rh);
    if (cfg_.integer_scale) scale = std::max(1.0f, std::floor(scale));

    const Rect dst{std::floor((win_w - rw * scale) * 0.5f), std::floor((win_h - rh * scale) * 0.5f), rw * scale,
                   rh * scale};

    renderer_->set_camera(Camera{});
    renderer_->clear(rgb(0, 0, 0));
    SpriteOptions o;
    o.scale_x = scale;
    o.scale_y = scale;
    renderer_->draw_texture(*target_, dst.x, dst.y, o);
    renderer_->set_presentation(dst, scale);
}

void App::render_frame(float alpha) {
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    const bool drawing = renderer_->begin_frame();
    // A screenshot of a window-sized game needs an offscreen copy to read back.
    Texture* shot = nullptr;
    if (screenshot_pending_ && drawing && !target_) {
        shot = renderer_->create_render_target("__screenshot", renderer_->window_width(), renderer_->window_height());
        if (shot) renderer_->set_target(shot);
    }
    if (target_) renderer_->set_target(target_);
    {
        RW_PROFILE_SCOPE("game.render");
        game_->render(*renderer_, alpha);
    }
    if (scene_) {
        RW_PROFILE_SCOPE("scene tools");
        scene_(*this, *renderer_);
    }
    if (target_) {
        if (cfg_.embedded) {
            present_to_view();
        } else {
            renderer_->set_target(nullptr);
            present_target();
        }
    } else if (shot) {
        renderer_->set_target(nullptr);
        renderer_->set_camera(Camera{});
        renderer_->draw_texture(*shot, 0, 0);
    }

    if (time_.show_overlay) {
        RW_PROFILE_SCOPE("overlay");
        draw_debug_overlay(timeline_, time_, *game_, stats_, hot_, this);
    }
    if (ui_) {
        RW_PROFILE_SCOPE("host ui");
        ui_(*this);
    }

    ImGui::Render();
    {
        RW_PROFILE_SCOPE("submit");
        renderer_->end_frame(drawing ? ImGui::GetDrawData() : nullptr);
    }

    if (screenshot_pending_ && drawing) {
        screenshot_pending_ = false;
        Texture* source = target_ ? target_ : shot;
        std::vector<u8> rgba;
        if (source && renderer_->read_texture(*source, rgba)) {
            std::string file = screenshot_file_;
            if (file.empty()) {
                const std::string dir = user_data_dir(cfg_.organization, cfg_.title) + "screenshots/";
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                const u64 now = static_cast<u64>(SDL_GetTicksNS() / 1000000ull);
                file = dir + "shot_" + format_timestamp(static_cast<u64>(std::time(nullptr))) + "_" + std::to_string(now % 1000) + ".png";
                for (char& c : file)
                    if (c == ':' || c == ' ') c = '-';
            }
            std::string error;
            if (write_png(file, source->width, source->height, rgba.data(), &error)) RW_LOG_INFO("app", "screenshot: %s", file.c_str());
            else RW_LOG_ERROR("app", "screenshot: %s", error.c_str());
        } else {
            RW_LOG_ERROR("app", "screenshot: could not read the frame back");
        }
    }
}

// ---- save games ------------------------------------------------------------------------

void App::restart(net::Session* session) {
    if (!game_) return;
    game_->init();
    if (start_) start_(*this);
    timeline_.reset(*game_);
    session_ = session;
    accumulator_ = 0;
    time_.mode = TimeMode::Playing;
    RW_LOG_INFO("app", "restarted%s", session ? " with a network session" : "");
}

std::string App::save_dir() const { return savegame_dir(cfg_.organization, cfg_.title); }

bool App::save_game(const std::string& slot, const std::string& label, std::string* error) {
    const std::string dir = save_dir();
    if (dir.empty() || !game_) {
        if (error) *error = "no save folder or no game";
        return false;
    }
    const bool ok = write_savegame(dir + slot + ".rwsave", *game_, cfg_.save_version, label, timeline_.head(), error);
    if (ok) RW_LOG_INFO("app", "saved game '%s'", slot.c_str());
    return ok;
}

bool App::load_game(const std::string& slot, std::string* error) {
    const std::string dir = save_dir();
    if (dir.empty() || !game_) {
        if (error) *error = "no save folder or no game";
        return false;
    }
    Game* game = game_;
    const SaveMigration migrate = [game](u32 from, std::vector<u8>& state) { return game->migrate_save(from, state); };
    SaveInfo info;
    if (!read_savegame(dir + slot + ".rwsave", *game_, cfg_.save_version, migrate, &info, error)) return false;
    timeline_.recapture(*game_);
    accumulator_ = 0;
    if (time_.mode == TimeMode::Replaying || time_.mode == TimeMode::Rewinding) time_.mode = TimeMode::Paused;
    RW_LOG_INFO("app", "loaded game '%s' (%s, saved %s)", slot.c_str(), info.label.c_str(), format_timestamp(info.timestamp).c_str());
    return true;
}

std::vector<SaveInfo> App::saved_games() const { return list_savegames(save_dir()); }

void App::screenshot(const std::string& file) {
    screenshot_pending_ = true;
    screenshot_file_ = file;
}

void App::set_view_size(int width, int height) {
    view_w_ = std::max(width, 1);
    view_h_ = std::max(height, 1);
}

void App::set_view_rect(const Rect& rect) {
    if (!target_ || rect.w <= 0) return;
    renderer_->set_presentation(rect, rect.w / static_cast<float>(target_->width));
}

void App::present_to_view() {
    // Scale the game's target into the view texture the host shows. Done here,
    // with our own sampler, so pixel art stays crisp regardless of how the
    // host's UI toolkit samples textures.
    if (view_w_ <= 0 || view_h_ <= 0) {
        renderer_->set_target(nullptr);
        return;
    }
    if (!view_ || view_->width != view_w_ || view_->height != view_h_) {
        view_ = renderer_->create_render_target("__view", view_w_, view_h_);
        if (!view_) return;
    }
    const float rw = static_cast<float>(target_->width);
    const float rh = static_cast<float>(target_->height);
    float scale = std::min(static_cast<float>(view_w_) / rw, static_cast<float>(view_h_) / rh);
    if (cfg_.integer_scale) scale = std::max(1.0f, std::floor(scale));
    const float x = std::floor((static_cast<float>(view_w_) - rw * scale) * 0.5f);
    const float y = std::floor((static_cast<float>(view_h_) - rh * scale) * 0.5f);

    renderer_->set_target(view_);
    renderer_->set_camera(Camera{});
    renderer_->clear(rgb(0, 0, 0));
    SpriteOptions o;
    o.scale_x = scale;
    o.scale_y = scale;
    renderer_->draw_texture(*target_, x, y, o);
    renderer_->set_target(nullptr);
}

// ---- player settings ---------------------------------------------------------------

void App::apply_player_settings() {
    if (window_) {
        set_fullscreen(settings_.fullscreen);
        if (device_) {
            SDL_GPUPresentMode present = SDL_GPU_PRESENTMODE_VSYNC;
            if (!settings_.vsync) {
                if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_MAILBOX))
                    present = SDL_GPU_PRESENTMODE_MAILBOX;
                else if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_IMMEDIATE))
                    present = SDL_GPU_PRESENTMODE_IMMEDIATE;
            }
            SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, present);
        }
    }
    audio_.set_mix(settings_.audio);
    active_bindings_ = !settings_.input.empty() ? settings_.input : !cfg_.input.empty() ? cfg_.input : InputBindings::defaults();
    compiled_ = compile_bindings(active_bindings_);
    for (const std::string& u : compiled_.unknown) RW_LOG_WARN("app", "input binding ignored: unknown %s", u.c_str());
}

bool App::save_player_settings(std::string* error) {
    return rw::save_player_settings(cfg_.organization, cfg_.title, settings_, error);
}

void App::set_fullscreen(bool on) {
    if (window_) SDL_SetWindowFullscreen(window_, on);
}

bool App::fullscreen() const { return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0; }

std::string App::take_pressed_key() {
    std::string k = std::move(pressed_key_);
    pressed_key_.clear();
    return k;
}

std::string App::take_pressed_gamepad_button() {
    std::string b = std::move(pressed_pad_);
    pressed_pad_.clear();
    return b;
}

bool App::load_state(const std::vector<u8>& bytes) {
    Reader reader(bytes);
    game_->load(reader);
    if (!reader.ok() || reader.remaining() != 0) return false;
    timeline_.recapture(*game_);
    accumulator_ = 0;
    return true;
}

int App::run(Game& game) {
    game_ = &game;
    module_ = nullptr;
    hot_ = HotReloadInfo{};
    return loop();
}

int App::run(GameModule& module) {
    if (!module.game()) {
        RW_LOG_WARN("app", "App::run: the game module is not loaded (%s)", module.error().c_str());
        return 1;
    }
    game_ = module.game();
    module_ = &module;
    hot_ = HotReloadInfo{};
    hot_.enabled = true;
    hot_.module_name = module.path().filename().string();
    hot_.generation = module.generation();
    return loop();
}

int App::loop() {
    if (!init_platform()) return 1;

    game_->start(*this);
    game_->load_assets(*renderer_, audio_);
    game_->init();
    if (start_) start_(*this);
    timeline_.reset(*game_);
    running_ = true;
    stats_.gpu_driver = renderer_->driver_name();

    u64 prev = SDL_GetTicksNS();
    double fps_window = 0;
    int fps_frames = 0;

    Profiler& profiler = Profiler::instance();
    u32 frame_number = 0;

    while (running_) {
        log_set_frame(++frame_number);
        profiler.begin_frame();
        const u64 now = SDL_GetTicksNS();
        double frame_dt = static_cast<double>(now - prev) / 1e9;
        prev = now;
        frame_dt = std::min(frame_dt, 0.25); // never spiral after a stall

        {
            RW_PROFILE_SCOPE("events");
            poll_hot_reload();
            poll_assets();
            handle_events();
        }
        const InputFrame live = sample_input();
        apply_requests(live);

        const u64 sim_start = SDL_GetTicksNS();
        {
            RW_PROFILE_SCOPE("simulate");
            simulate(live, frame_dt);
        }
        stats_.sim_ms = static_cast<float>(SDL_GetTicksNS() - sim_start) / 1e6f;

        {
            RW_PROFILE_SCOPE("audio");
            dispatch_events();
            audio_.update();
        }
        stats_.audio_voices = audio_.active_voices();
        hot_.tracked_files = assets_.count();

        const u64 render_start = SDL_GetTicksNS();
        const float alpha = time_.mode == TimeMode::Playing
                                ? static_cast<float>(accumulator_ / timeline_.dt())
                                : 0.0f;
        {
            RW_PROFILE_SCOPE("render");
            render_frame(alpha);
        }
        stats_.render_ms = static_cast<float>(SDL_GetTicksNS() - render_start) / 1e6f;
        stats_.draw_calls = renderer_->draw_calls_last_frame();
        stats_.vertices = renderer_->vertices_last_frame();
        stats_.net = session_ ? &session_->stats() : nullptr;
        profiler.end_frame();

        stats_.frame_ms = static_cast<float>(frame_dt * 1000.0);
        fps_window += frame_dt;
        ++fps_frames;
        if (fps_window >= 0.5) {
            stats_.fps = static_cast<float>(fps_frames / fps_window);
            fps_window = 0;
            fps_frames = 0;
        }
    }

    shutdown_platform();
    return 0;
}

} // namespace rw
