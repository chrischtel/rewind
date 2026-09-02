#pragma once

#include "rewind/core/types.h"
#include "rewind/net/transport.h"

#include <string>

namespace rw::net {

/// Plain UDP to a single peer. Bind a local port; either name the peer up
/// front or leave the host empty and adopt whoever sends the first packet
/// (the "host" side of a two-player session).
class UdpTransport final : public Transport {
public:
    UdpTransport();
    ~UdpTransport() override;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    bool open(u16 local_port, const std::string& peer_host = "", u16 peer_port = 0);
    void close();
    bool is_open() const { return socket_ != kInvalid; }
    bool has_peer() const { return has_peer_; }
    const std::string& error() const { return error_; }

    void send(const std::vector<u8>& packet) override;
    bool receive(std::vector<u8>& out) override;

    u32 packets_sent() const { return sent_; }
    u32 packets_received() const { return received_; }

private:
    static constexpr long long kInvalid = -1;

    long long socket_ = kInvalid;
    bool has_peer_ = false;
    u8 peer_addr_[32] = {};
    int peer_len_ = 0;
    std::string error_;
    u32 sent_ = 0;
    u32 received_ = 0;
};

} // namespace rw::net
