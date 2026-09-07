#pragma once

#include "rewind/core/types.h"
#include "rewind/net/transport.h"
#include "rewind/sim/input.h"

#include <functional>
#include <deque>
#include <vector>

namespace rw {
class Game;
class Timeline;
struct EventQueue;
} // namespace rw

namespace rw::net {

struct SessionConfig {
    u32 player_count = 2;
    u32 local_player = 0;
    u32 input_delay = 2;   ///< Ticks between pressing a button and it taking effect. Hides latency.
    u32 max_rollback = 8;  ///< How far ahead of confirmed remote input we may run before stalling.
    u32 sync_interval = 20; ///< Ticks between state fingerprints exchanged for desync detection.
    u32 redundancy = 12;   ///< Recent inputs repeated in every packet so losses need no retransmit.
};

struct SessionStats {
    bool connected = false;      ///< Something was received from a peer.
    u32 connected_players = 0;   ///< Remote players heard from so far.
    u32 rollbacks = 0;           ///< Times a wrong prediction forced a re-simulation.
    u32 rolled_back_ticks = 0;   ///< Ticks re-simulated in total.
    u32 stalled_frames = 0;      ///< Frames that could not advance because the peer fell behind.
    u32 predicted_ticks = 0;     ///< Ticks currently simulated on guessed remote input.
    Tick remote_confirmed = 0;   ///< Remote input is known for every tick below this.
    u32 packets_sent = 0;
    u32 packets_received = 0;
    float rtt_ms = 0;            ///< Round trip estimate from echoed timestamps.
    bool desync = false;         ///< Peers disagreed on a confirmed state. Determinism bug.
    Tick desync_tick = 0;
};

/// The wire format of one packet, exposed for tests.
struct Packet {
    u8 player = 0;          ///< Sender's player id.
    u32 start_tick = 0;     ///< Tick of frames[0].
    std::vector<InputFrame> frames;
    u32 ack = 0;            ///< Sender knows the receiver's input for every tick below this.
    u32 time_ms = 0;        ///< Sender's clock, echoed back for RTT.
    u32 echo_ms = 0;        ///< Latest time_ms received from the peer, 0 if none.
    bool has_sync = false;
    u32 sync_tick = 0;
    u64 sync_hash = 0;
};

void encode_packet(const Packet& packet, std::vector<u8>& out);
bool decode_packet(const std::vector<u8>& bytes, Packet& out);

/// Rollback netcode over the Timeline for up to kMaxPlayers players.
///
/// Every tick needs every player's input. The local one is known ahead of
/// time thanks to `input_delay`; the remote one is predicted (last known)
/// until it arrives. When a real input contradicts a prediction, the session
/// corrects the recorded tick and re-simulates from there, which the
/// timeline already knows how to do. Inputs are sent redundantly, so lost
/// packets need no retransmission. Confirmed states are fingerprinted and
/// compared so a determinism bug shows up as `stats().desync`.
class Session {
public:
    Session(const SessionConfig& config, Transport& transport);

    /// Exchange packets, roll back if needed, then advance up to
    /// `wanted_ticks` (0 is fine: the exchange still happens). Returns how
    /// many ticks were advanced. `now_ms` is any monotonic clock.
    u32 step(Timeline& timeline, Game& game, const InputFrame& local, u32 wanted_ticks, EventQueue* events,
             u64 now_ms);
    /// Same, with a fresh local input per advanced tick (App uses it so a
    /// queued command lands in exactly one tick).
    u32 step(Timeline& timeline, Game& game, const std::function<InputFrame()>& next_local, u32 wanted_ticks,
             EventQueue* events, u64 now_ms);

    const SessionConfig& config() const { return config_; }
    const SessionStats& stats() const { return stats_; }

    /// Every player's input is known for all ticks below this.
    Tick confirmed_tick() const;

private:
    struct PlayerInputs {
        Tick base = 0;
        Tick known_until = 0; ///< frames cover [base, known_until)
        std::vector<InputFrame> frames;
        InputFrame last{};

        bool known(Tick t) const { return t >= base && t < known_until; }
        const InputFrame& at(Tick t) const { return frames[static_cast<usize>(t - base)]; }
        void append(Tick t, const InputFrame& f);
        void trim(Tick keep_from);
    };

    void add_local(Tick tick, const InputFrame& input);
    InputSet inputs_for(Tick tick, bool& predicted) const;
    void receive(Timeline& timeline, u64 now_ms);
    void send(Timeline& timeline, u64 now_ms);
    void check_syncs(Timeline& timeline);

    SessionConfig config_;
    Transport& transport_;
    SessionStats stats_;
    std::vector<PlayerInputs> players_;
    Tick rollback_from_;
    std::vector<Tick> acks_;       ///< per player: they know our input for every tick below this
    std::vector<bool> heard_;
    Tick remote_ack_ = 0;          ///< min over the acks of every remote player
    u32 last_remote_time_ms_ = 0;
    u32 ticks_since_sync_ = 0;
    std::deque<std::pair<Tick, u64>> pending_syncs_;
    std::vector<u8> scratch_;
};

} // namespace rw::net
