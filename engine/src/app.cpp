#include "rewind/core/app.h"
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

    static const bool no_keys[SDL_SCANCODE_COUNT] = {};
    const bool* k = keyboard ? SDL_GetKeyboardState(nullptr) : no_keys;
    in.set(Button::Up, k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]);
    in.set(Button::Down, k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]);
    in.set(Button::Left, k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]);
    in.set(Button::Right, k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]);
    in.set(Button::A, k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_J] || k[SDL_SCANCODE_SPACE]);
    in.set(Button::B, k[SDL_SCANCODE_X] || k[SDL_SCANCODE_K] || k[SDL_SCANCODE_LSHIFT]);
    in.set(Button::X, k[SDL_SCANCODE_C] || k[SDL_SCANCODE_L]);
    in.set(Button::Y, k[SDL_SCANCODE_V] || k[SDL_SCANCODE_I]);
    in.set(Button::L, k[SDL_SCANCODE_Q]);
    in.set(Button::R, k[SDL_SCANCODE_E]);
    in.set(Button::Start, k[SDL_SCANCODE_RETURN]);
    in.set(Button::Select, k[SDL_SCANCODE_BACKSPACE]);

    i32 axis_x = 0;
    i32 axis_y = 0;

    if (gamepad_) {
        auto held = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(gamepad_, b); };
        auto set_or = [&](Button b, bool on) {
            if (on) in.set(b, true);
        };
        set_or(Button::Up, held(SDL_GAMEPAD_BUTTON_DPAD_UP));
        set_or(Button::Down, held(SDL_GAMEPAD_BUTTON_DPAD_DOWN));
        set_or(Button::Left, held(SDL_GAMEPAD_BUTTON_DPAD_LEFT));
        set_or(Button::Right, held(SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
        set_or(Button::A, held(SDL_GAMEPAD_BUTTON_SOUTH));
        set_or(Button::B, held(SDL_GAMEPAD_BUTTON_EAST));
        set_or(Button::X, held(SDL_GAMEPAD_BUTTON_WEST));
        set_or(Button::Y, held(SDL_GAMEPAD_BUTTON_NORTH));
        set_or(Button::L, held(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
        set_or(Button::R, held(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
        set_or(Button::Start, held(SDL_GAMEPAD_BUTTON_START));
        set_or(Button::Select, held(SDL_GAMEPAD_BUTTON_BACK));

        constexpr i32 kDeadzone = 8000;
        const i32 sx = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX);
        const i32 sy = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTY);
        if (std::abs(sx) > kDeadzone) axis_x = std::clamp(sx, -32767, 32767);
        if (std::abs(sy) > kDeadzone) axis_y = std::clamp(sy, -32767, 32767);
    }

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
    }

    if (time_.show_overlay) {
        RW_PROFILE_SCOPE("overlay");
        draw_debug_overlay(timeline_, time_, *game_, stats_, hot_);
    }
    if (ui_) {
        RW_PROFILE_SCOPE("host ui");
        ui_(*this);
    }

    ImGui::Render();
    RW_PROFILE_SCOPE("submit");
    renderer_->end_frame(drawing ? ImGui::GetDrawData() : nullptr);
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
