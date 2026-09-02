// Duel: two players over rollback netcode. Each player is a square; hold a
// direction to run, press Z / A to dash. Dashing into the other player
// scores a hit.
//
//   duel --host 7777                    wait for a peer on UDP port 7777 (you are player 0)
//   duel --connect 192.168.1.5:7777     join (you are player 1)
//   duel                                offline: player 1 is a bot
//
// Nothing in tick() knows about the network. It reads ctx.player(0) and
// ctx.player(1); the session fills those in, predicting the remote player
// and rolling back when the guess was wrong. Hold F4 to rewind offline.

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

} // namespace

class Duel final : public Game {
public:
    void configure(AppConfig& cfg) override {
        cfg.title = "Rewind - duel";
        cfg.width = static_cast<int>(kWidth);
        cfg.height = static_cast<int>(kHeight);
        cfg.timeline.history_ticks = 60 * 60;

        for (usize i = 0; i < cfg.args.size(); ++i) {
            const std::string& a = cfg.args[i];
            if (a == "--host" && i + 1 < cfg.args.size()) {
                host_port_ = static_cast<u16>(std::atoi(cfg.args[++i].c_str()));
                mode_ = Mode::Host;
            } else if (a == "--connect" && i + 1 < cfg.args.size()) {
                const std::string target = cfg.args[++i];
                const usize colon = target.find(':');
                connect_host_ = colon == std::string::npos ? target : target.substr(0, colon);
                connect_port_ = colon == std::string::npos ? 7777 : static_cast<u16>(std::atoi(target.c_str() + colon + 1));
                mode_ = Mode::Client;
            } else if (a == "--delay" && i + 1 < cfg.args.size()) {
                input_delay_ = static_cast<u32>(std::atoi(cfg.args[++i].c_str()));
            }
        }
    }

    void start(App& app) override {
        if (mode_ == Mode::Offline) return;
        udp_ = std::make_unique<net::UdpTransport>();
        const bool ok = mode_ == Mode::Host ? udp_->open(host_port_) : udp_->open(0, connect_host_, connect_port_);
        if (!ok) {
            RW_LOG_ERROR("duel", "network setup failed: %s (running offline)", udp_->error().c_str());
            udp_.reset();
            mode_ = Mode::Offline;
            return;
        }
        net::SessionConfig cfg;
        cfg.player_count = 2;
        cfg.local_player = mode_ == Mode::Host ? 0 : 1;
        cfg.input_delay = input_delay_;
        session_ = std::make_unique<net::Session>(cfg, *udp_);
        app.set_session(session_.get());
        RW_LOG_INFO("duel", "%s as player %u, input delay %u ticks", mode_ == Mode::Host ? "hosting" : "joining",
                    cfg.local_player, cfg.input_delay);
    }

    void init() override {
        fighters_[0] = Fighter{};
        fighters_[0].x = kWidth * 0.25f;
        fighters_[0].y = kHeight * 0.5f;
        fighters_[1] = Fighter{};
        fighters_[1].x = kWidth * 0.75f;
        fighters_[1].y = kHeight * 0.5f;
        tick_ = 0;
    }

    void tick(const TickContext& c) override {
        tick_ = c.tick;
        for (u32 p = 0; p < 2; ++p) {
            InputFrame in = c.player(p);
            if (mode_ == Mode::Offline && p == 1) in = bot_input(c.tick);
            step_fighter(fighters_[p], in, c);
        }
        resolve_hits(c);
    }

    void on_event(const SimEvent& e, Audio&) override {
        (void)e; // sounds would go here; the sample stays silent
    }

    void reflect(Visitor& v) override {
        v.field("p0", fighters_[0]);
        v.field("p1", fighters_[1]);
        v.field("tick", tick_, {.read_only = true});
    }

    void render(Renderer2D& r, float) override {
        r.clear(rgb(20, 22, 30));
        for (int y = 0; y < static_cast<int>(kHeight); y += 60)
            r.draw_line(0, static_cast<float>(y), kWidth, static_cast<float>(y), rgb(30, 33, 44));
        for (int x = 0; x < static_cast<int>(kWidth); x += 60)
            r.draw_line(static_cast<float>(x), 0, static_cast<float>(x), kHeight, rgb(30, 33, 44));

        const Color colors[2] = {rgb(90, 200, 255), rgb(255, 120, 90)};
        for (u32 p = 0; p < 2; ++p) {
            const Fighter& f = fighters_[p];
            Color c = colors[p];
            if (f.dash_ticks > 0) c = rgb(255, 255, 255);
            r.fill_rect(f.x - kSize * 0.5f, f.y - kSize * 0.5f, kSize, kSize, c);
            r.draw_rect(f.x - kSize * 0.5f, f.y - kSize * 0.5f, kSize, kSize, rgb(255, 255, 255));
        }

        char buf[128];
        std::snprintf(buf, sizeof buf, "P0 hits %u    P1 hits %u", fighters_[0].hits, fighters_[1].hits);
        r.draw_text(12, 12, buf, rgb(220, 220, 230), 2);
        const char* mode = mode_ == Mode::Host ? "hosting" : (mode_ == Mode::Client ? "client" : "offline (bot)");
        std::snprintf(buf, sizeof buf, "%s   arrows: move   Z: dash", mode);
        r.draw_text(12, kHeight - 20, buf, rgb(150, 150, 170));
    }

    void debug_ui() override {
        ImGui::Text("mode: %s", mode_ == Mode::Host ? "host" : (mode_ == Mode::Client ? "client" : "offline"));
        ImGui::TextWrapped("start two copies: one with --host 7777, one with --connect 127.0.0.1:7777. "
                           "The overlay line above shows rollbacks and rtt.");
    }

private:
    enum class Mode { Offline, Host, Client };

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
            // Keep dashing in the last direction of travel.
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
        Fighter& a = fighters_[0];
        Fighter& b = fighters_[1];
        const bool touching = absf(a.x - b.x) < kSize && absf(a.y - b.y) < kSize;
        if (!touching) return;
        if (a.dash_ticks > 0 && b.dash_ticks == 0) {
            ++a.hits;
            a.dash_ticks = 0;
            b.x = clampf(b.x + (b.x - a.x) * 2.0f, kSize, kWidth - kSize);
            c.emit(kEvHit, 0);
        } else if (b.dash_ticks > 0 && a.dash_ticks == 0) {
            ++b.hits;
            b.dash_ticks = 0;
            a.x = clampf(a.x + (a.x - b.x) * 2.0f, kSize, kWidth - kSize);
            c.emit(kEvHit, 1);
        }
    }

    static InputFrame bot_input(Tick tick) {
        // Wanders in a slow figure of eight and dashes every few seconds.
        InputFrame in;
        const float t = static_cast<float>(tick) / 60.0f;
        in.axis_x = static_cast<i16>(std::sin(t * 0.7f) * 32767.0f);
        in.axis_y = static_cast<i16>(std::sin(t * 1.4f) * 32767.0f);
        in.set(Button::A, tick % 240 == 0);
        return in;
    }

    Mode mode_ = Mode::Offline;
    u16 host_port_ = 7777;
    std::string connect_host_;
    u16 connect_port_ = 7777;
    u32 input_delay_ = 2;
    std::unique_ptr<net::UdpTransport> udp_;
    std::unique_ptr<net::Session> session_;

    Fighter fighters_[2];
    Tick tick_ = 0;
};

REWIND_GAME(Duel)
