#pragma once

#include "rewind/core/types.h"
#include "rewind/net/transport.h"

#include <string>
#include <vector>

namespace rw::net {

/// Plain UDP on one socket to any number of peers. Bind a local port, then
/// add peers (a lobby does that for you) or leave the list empty and adopt
/// whoever sends the first packet (the "host" side of a two-player game).
class UdpTransport final : public AddressedTransport {
public:
    UdpTransport();
    ~UdpTransport() override;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    /// Bind `local_port` (0 = any). A peer given here is added right away.
    bool open(u16 local_port, const std::string& peer_host = "", u16 peer_port = 0);
    void close();
    bool is_open() const { return socket_ != kInvalid; }
    bool has_peer() const { return !peers_.empty(); }
    u16 local_port() const { return local_port_; }
    const std::string& error() const { return error_; }

    void send(const std::vector<u8>& packet) override;
    void send_to(const std::vector<u8>& packet, const Address& to) override;
    bool receive_from(std::vector<u8>& out, Address& from) override;
    void add_peer(const Address& peer) override;
    void clear_peers() override { peers_.clear(); }
    const std::vector<Address>& peers() const override { return peers_; }

    /// Whether unknown senders become peers (on until a peer was added).
    void set_adopt_first_sender(bool on) { adopt_ = on; }

    u32 packets_sent() const { return sent_; }
    u32 packets_received() const { return received_; }

private:
    static constexpr long long kInvalid = -1;
    bool resolve(const Address& address, u8* out, int& len);

    long long socket_ = kInvalid;
    u16 local_port_ = 0;
    std::vector<Address> peers_;
    bool adopt_ = true;
    std::string error_;
    u32 sent_ = 0;
    u32 received_ = 0;
};

} // namespace rw::net
