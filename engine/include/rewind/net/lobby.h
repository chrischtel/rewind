#pragma once

// A lobby before a rollback session: one player hosts, others join by
// address, everyone sees the slot list and ready flags, the host starts a
// countdown, and each peer ends up with the same player count, its own
// slot, a shared seed and the other players' addresses. Runs over an
// AddressedTransport (UDP or the in-process mesh) with small repeated
// messages, so loss needs no special handling.
//
// Once started, hand the transport to a Session: connect_peers() adds
// every other player as a peer and session_config() fills the rest in.

#include "rewind/core/types.h"
#include "rewind/net/session.h"
#include "rewind/net/transport.h"

#include <string>
#include <vector>

namespace rw::net {

struct LobbyPlayer {
    std::string name;
    Address address;   ///< as the host sees it; the host's own slot holds the address clients joined
    bool present = false;
    bool ready = false;
    u64 last_seen_ms = 0;
};

class Lobby {
public:
    enum class State { Idle, Joining, Waiting, Starting, Started, Failed };

    explicit Lobby(AddressedTransport& transport);

    void host(const std::string& name, u32 max_players);
    void join(const Address& host_address, const std::string& name);
    void leave();

    /// Send and receive lobby traffic. Call every frame with a wall clock.
    void update(u64 now_ms);
    void set_ready(bool ready);
    /// Host only, all present players ready: broadcast the start. `seed` 0 picks one from the clock.
    bool start(u64 now_ms, u32 countdown_ms = 3000, u64 seed = 0);

    State state() const { return state_; }
    bool is_host() const { return host_; }
    u32 local_slot() const { return local_slot_; }
    u32 max_players() const { return static_cast<u32>(players_.size()); }
    u32 player_count() const;
    bool all_ready() const;
    const std::vector<LobbyPlayer>& players() const { return players_; }
    const std::string& error() const { return error_; }
    u64 seed() const { return seed_; }
    /// Milliseconds until the game starts while Starting, else 0.
    u32 countdown_ms(u64 now_ms) const;

    /// After Started: the other players as transport peers, and the config.
    void connect_peers(AddressedTransport& transport) const;
    std::vector<Address> peer_addresses() const;
    SessionConfig session_config(u32 input_delay = 2) const;

    /// Wire messages, exposed for tests.
    enum class Msg : u8 { Join = 1, Welcome, State, Ready, Start, Leave };
    static bool is_lobby_packet(const std::vector<u8>& bytes);

private:
    void handle(const std::vector<u8>& bytes, const Address& from, u64 now_ms);
    void send_state(u64 now_ms);
    void send_join();
    void send_ready();
    void send_start();
    void fail(const std::string& why);

    AddressedTransport& transport_;
    State state_ = State::Idle;
    bool host_ = false;
    std::string name_;
    u32 local_slot_ = 0;
    std::vector<LobbyPlayer> players_;
    Address host_address_;
    bool ready_ = false;
    u64 seed_ = 0;
    u64 start_at_ms_ = 0;
    u64 last_send_ms_ = 0;
    u64 joined_ms_ = 0;
    std::string error_;
    std::vector<u8> scratch_;
};

} // namespace rw::net
