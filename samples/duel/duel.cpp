// Arena: up to four players over rollback netcode, with a lobby. Each player
// is a square; hold a direction to run, press Z / A to dash. Dashing into
// another player scores a hit.
//
//   duel --host 7777 [--name Ann] [--players 4]   host a lobby on UDP port 7777 (you are player 0)
//   duel --join 192.168.1.5:7777 [--name Bob]      join it
//   duel                                            offline: the other players are bots
//
// In the lobby: Z toggles ready, the host presses Enter when everyone is
// ready. Nothing in tick() knows about the network: it reads ctx.player(i)
// for every slot; the session fills those in, predicting remote players
// and rolling back when a guess was wrong. Hold F4 to rewind offline.

#include <rewind/rewind.h>

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using namespace rw;

namespace {

constexpr float kWidth = 960;
constexpr float kHeight = 540;
constexpr float kSize = 36;

struct Fighter {
    float x = 0, y = 0;
    float vx = 0, vy = 0;
    u32 dash_ticks = 0;    ///< remaining ticks of the current dash
    u32 cooldown = 0;
    u32 hits = 0;
    InputFrame prev;

    void reflect(Visitor& v) {
        v.field("x", x);
        v.field("y", y);
        v.field("vx", vx);
        v.field("vy", vy);
        v.field("dash_ticks", dash_ticks);
        v.field("cooldown", cooldown);
        v.field("hits", hits);
        v.field("prev", prev);
    }
};

enum Event : u32 { kEvDash = 1, kEvHit = 2 };

const Color kColors[kMaxPlayers] = {rgb(90, 200, 255), rgb(255, 120, 90), rgb(120, 230, 120), rgb(240, 200, 80)};

} // namespace

class Arena final : public Game {
public:
    void configure(AppConfig& cfg) override {
        cfg.title = "Rewind - arena";
        cfg.width = static_cast<int>(kWidth);
        cfg.height = static_cast<int>(kHeight);
        cfg.timeline.history_ticks = 60 * 60;

        for (usize i = 0; i < cfg.args.size(); ++i) {
            const std::string& a = cfg.args[i];
            if (a == "--host" && i + 1 < cfg.args.size()) {
                host_port_ = static_cast<u16>(std::atoi(cfg.args[++i].c_str()));
                mode_ = Mode::Host;
            } else if ((a == "--join" || a == "--connect") && i + 1 < cfg.args.size()) {
                const std::string target = cfg.args[++i];
                const usize colon = target.find(':');
                join_.host = colon == std::string::npos ? target : target.substr(0, colon);
                join_.port = colon == std::string::npos ? 7777 : static_cast<u16>(std::atoi(target.c_str() + colon + 1));
                mode_ = Mode::Client;
            } else if (a == "--name" && i + 1 < cfg.args.size()) {
                name_ = cfg.args[++i];
            } else if (a == "--players" && i + 1 < cfg.args.size()) {
                max_players_ = static_cast<u32>(std::clamp(std::atoi(cfg.args[++i].c_str()), 1, static_cast<int>(kMaxPlayers)));
            } else if (a == "--delay" && i + 1 < cfg.args.size()) {
                input_delay_ = static_cast<u32>(std::atoi(cfg.args[++i].c_str()));
            }
        }
        if (name_.empty()) name_ = mode_ == Mode::Host ? "host" : "player";
    }

    void start(App& app) override {
        app_ = &app;
        if (mode_ == Mode::Offline) return;
        udp_ = std::make_unique<net::UdpTransport>();
        udp_->set_adopt_first_sender(false);
        if (!udp_->open(mode_ == Mode::Host ? host_port_ : 0)) {
            RW_LOG_ERROR("arena", "network setup failed: %s (running offline)", udp_->error().c_str());
            udp_.reset();
            mode_ = Mode::Offline;
            return;
        }
        lobby_ = std::make_unique<net::Lobby>(*udp_);
        if (mode_ == Mode::Host) lobby_->host(name_, max_players_);
        else lobby_->join(join_, name_);
        in_lobby_ = true;
    }

    void init() override {
        player_count_ = session_ ? session_->config().player_count : (mode_ == Mode::Offline ? max_players_ : 1);
        for (u32 p = 0; p < kMaxPlayers; ++p) {
            fighters_[p] = Fighter{};
            const float angle = static_cast<float>(p) / static_cast<float>(std::max<u32>(player_count_, 1)) * 6.2831853f;
            fighters_[p].x = kWidth * 0.5f + std::cos(angle) * kWidth * 0.3f;
            fighters_[p].y = kHeight * 0.5f + std::sin(angle) * kHeight * 0.3f;
        }
        tick_ = 0;
    }

    void tick(const TickContext& c) override {
        if (in_lobby_) return; // the lobby is not part of the simulation
        tick_ = c.tick;
        for (u32 p = 0; p < player_count_; ++p) {
            InputFrame in = c.player(p);
            if (mode_ == Mode::Offline && p > 0) in = bot_input(c.tick, p);
            step_fighter(fighters_[p], in, c);
        }
        resolve_hits(c);
    }

    void on_event(const SimEvent& e, Audio&) override {
        (void)e; // sounds would go here; the sample stays silent
    }

    void reflect(Visitor& v) override {
        v.field("player_count", player_count_, {.min = 1, .max = static_cast<float>(kMaxPlayers)});
        for (u32 p = 0; p < kMaxPlayers; ++p) v.field("fighter", fighters_[p]);
        v.field("tick", tick_, {.read_only = true});
    }

    void render(Renderer2D& r, float) override {
        r.clear(rgb(20, 22, 30));
        for (int y = 0; y < static_cast<int>(kHeight); y += 60)
            r.draw_line(0, static_cast<float>(y), kWidth, static_cast<float>(y), rgb(30, 33, 44));
        for (int x = 0; x < static_cast<int>(kWidth); x += 60)
            r.draw_line(static_cast<float>(x), 0, static_cast<float>(x), kHeight, rgb(30, 33, 44));

        if (in_lobby_) {
            run_lobby(r);
            return;
        }

        for (u32 p = 0; p < player_count_; ++p) {
            const Fighter& f = fighters_[p];
            Color c = kColors[p];
            if (f.dash_ticks > 0) c = rgb(255, 255, 255);
            r.fill_rect(f.x - kSize * 0.5f, f.y - kSize * 0.5f, kSize, kSize, c);
            r.draw_rect(f.x - kSize * 0.5f, f.y - kSize * 0.5f, kSize, kSize, rgb(255, 255, 255));
        }

        char buf[160];
        int n = 0;
        for (u32 p = 0; p < player_count_; ++p)
            n += std::snprintf(buf + n, sizeof buf - static_cast<usize>(n), "P%u %u   ", p, fighters_[p].hits);
        r.draw_text(12, 12, buf, rgb(220, 220, 230), 2);
        const char* mode = mode_ == Mode::Host ? "hosting" : (mode_ == Mode::Client ? "client" : "offline (bots)");
        std::snprintf(buf, sizeof buf, "%s   arrows: move   Z: dash", mode);
        r.draw_text(12, kHeight - 20, buf, rgb(150, 150, 170));
    }

    void debug_ui() override {
        ImGui::Text("mode: %s", mode_ == Mode::Host ? "host" : (mode_ == Mode::Client ? "client" : "offline"));
        ImGui::TextWrapped("start copies with --host 7777 and --join 127.0.0.1:7777 (up to four). "
                           "The overlay line above shows rollbacks and rtt.");
    }

private:
    enum class Mode { Offline, Host, Client };

    /// The lobby runs in render(): it is wall-clock, not simulation.
    void run_lobby(Renderer2D& r) {
        const u64 now = static_cast<u64>(ImGui::GetTime() * 1000.0);
        lobby_->update(now);
        const std::string key = app_->take_pressed_key();
        const bool seated = lobby_->local_slot() < lobby_->players().size();
        if ((key == "Z" || key == "Space") && seated) lobby_->set_ready(!lobby_->players()[lobby_->local_slot()].ready);
        if (key == "Return" && lobby_->is_host() && !lobby_->start(now)) RW_LOG_WARN("arena", "not everyone is ready");

        r.draw_text(12, 12, lobby_->is_host() ? "LOBBY (hosting)" : "LOBBY", rgb(220, 220, 230), 2);
        float y = 60;
        for (usize i = 0; i < lobby_->players().size(); ++i) {
            const net::LobbyPlayer& p = lobby_->players()[i];
            char line[128];
            if (p.present) std::snprintf(line, sizeof line, "P%zu  %-16s %s", i, p.name.c_str(), p.ready ? "READY" : "not ready");
            else std::snprintf(line, sizeof line, "P%zu  (open)", i);
            r.draw_text(24, y, line, p.present ? kColors[i] : rgb(90, 90, 100), 2);
            y += 24;
        }
        const u32 countdown = lobby_->countdown_ms(now);
        char status[160];
        switch (lobby_->state()) {
        case net::Lobby::State::Joining: std::snprintf(status, sizeof status, "connecting to %s:%u ...", join_.host.c_str(), join_.port); break;
        case net::Lobby::State::Starting: std::snprintf(status, sizeof status, "starting in %.1f s", static_cast<float>(countdown) / 1000.0f); break;
        case net::Lobby::State::Failed: std::snprintf(status, sizeof status, "failed: %s", lobby_->error().c_str()); break;
        default:
            std::snprintf(status, sizeof status, lobby_->is_host() ? "Z: ready   Enter: start when everyone is ready" : "Z: ready   waiting for the host");
        }
        r.draw_text(12, kHeight - 20, status, rgb(150, 150, 170));

        if (lobby_->state() == net::Lobby::State::Started) {
            // Same roster, same seed, tick 0 on every peer: the session takes over.
            lobby_->connect_peers(*udp_);
            session_ = std::make_unique<net::Session>(lobby_->session_config(input_delay_), *udp_);
            in_lobby_ = false;
            app_->restart(session_.get());
            RW_LOG_INFO("arena", "playing as player %u of %u", session_->config().local_player, session_->config().player_count);
        }
    }

    static void step_fighter(Fighter& f, const InputFrame& in, const TickContext& c) {
        const float dt = c.dt;
        if (f.cooldown > 0) --f.cooldown;
        if (pressed(in, f.prev, Button::A) && f.cooldown == 0 && f.dash_ticks == 0) {
            f.dash_ticks = 10;
            f.cooldown = 40;
            c.emit(kEvDash);
        }
        f.prev = in;

        const float ax = static_cast<float>(in.axis_x) / 32767.0f;
        const float ay = static_cast<float>(in.axis_y) / 32767.0f;
        const float speed = f.dash_ticks > 0 ? 900.0f : 260.0f;
        if (f.dash_ticks > 0) {
            --f.dash_ticks;
            const float len = std::sqrt(f.vx * f.vx + f.vy * f.vy);
            if (len > 1.0f) {
                f.vx = f.vx / len * speed;
                f.vy = f.vy / len * speed;
            } else {
                f.vx = speed;
            }
        } else {
            f.vx = ax * speed;
            f.vy = ay * speed;
        }
        f.x = clampf(f.x + f.vx * dt, kSize * 0.5f, kWidth - kSize * 0.5f);
        f.y = clampf(f.y + f.vy * dt, kSize * 0.5f, kHeight - kSize * 0.5f);
    }

    void resolve_hits(const TickContext& c) {
        for (u32 i = 0; i < player_count_; ++i)
            for (u32 j = 0; j < player_count_; ++j) {
                if (i == j) continue;
                Fighter& a = fighters_[i];
                Fighter& b = fighters_[j];
                const bool touching = absf(a.x - b.x) < kSize && absf(a.y - b.y) < kSize;
                if (!touching || a.dash_ticks == 0 || b.dash_ticks > 0) continue;
                ++a.hits;
                a.dash_ticks = 0;
                b.x = clampf(b.x + (b.x - a.x) * 2.0f, kSize, kWidth - kSize);
                b.y = clampf(b.y + (b.y - a.y) * 2.0f, kSize, kHeight - kSize);
                c.emit(kEvHit, static_cast<float>(i));
            }
    }

    static InputFrame bot_input(Tick tick, u32 player) {
        // Wanders in a slow figure of eight and dashes every few seconds.
        InputFrame in;
        const float t = static_cast<float>(tick) / 60.0f + static_cast<float>(player) * 1.7f;
        in.axis_x = static_cast<i16>(std::sin(t * 0.7f) * 32767.0f);
        in.axis_y = static_cast<i16>(std::sin(t * 1.4f) * 32767.0f);
        in.set(Button::A, (tick + player * 60) % 240 == 0);
        return in;
    }

    Mode mode_ = Mode::Offline;
    u16 host_port_ = 7777;
    net::Address join_;
    std::string name_;
    u32 max_players_ = 4;
    u32 input_delay_ = 2;
    App* app_ = nullptr;
    std::unique_ptr<net::UdpTransport> udp_;
    std::unique_ptr<net::Lobby> lobby_;
    std::unique_ptr<net::Session> session_;
    bool in_lobby_ = false;

    u32 player_count_ = 1;
    Fighter fighters_[kMaxPlayers];
    Tick tick_ = 0;
};

REWIND_GAME(Arena)
