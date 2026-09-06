#pragma once

#include "rewind/core/types.h"

#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace rw::net {

/// Unreliable, unordered datagrams. UDP in real life, a queue with fake
/// latency in tests. Sessions never assume delivery. send() reaches every
/// peer the transport knows; receive() takes from any of them.
class Transport {
public:
    virtual ~Transport() = default;
    virtual void send(const std::vector<u8>& packet) = 0;
    /// Next packet, if any. Returns false when nothing is waiting.
    virtual bool receive(std::vector<u8>& out) = 0;
};

/// Where a peer is. For UDP a host name or dotted address and a port; for
/// the in-process mesh the end index as the port.
struct Address {
    std::string host;
    u16 port = 0;

    bool valid() const { return port != 0 || !host.empty(); }
    bool operator==(const Address&) const = default;
};

/// A transport that can also talk to a specific peer and tell who sent a
/// packet. Lobbies use this to hand out slots and share peer lists.
class AddressedTransport : public Transport {
public:
    virtual void send_to(const std::vector<u8>& packet, const Address& to) = 0;
    virtual bool receive_from(std::vector<u8>& out, Address& from) = 0;
    /// Peers that send() reaches; the session's other players.
    virtual void add_peer(const Address& peer) = 0;
    virtual void clear_peers() = 0;
    virtual const std::vector<Address>& peers() const = 0;

    bool receive(std::vector<u8>& out) override {
        Address ignored;
        return receive_from(out, ignored);
    }
};

/// Two transports wired to each other in-process, with simulated one-way
/// latency, jitter and loss driven by a virtual clock. Deterministic, so a
/// test can replay the exact same network misbehaviour every time.
class LoopbackPair {
public:
    LoopbackPair();
    ~LoopbackPair();

    Transport& a();
    Transport& b();

    /// Fixed base latency, random extra up to `jitter_ms`, and loss in [0, 1).
    void set_conditions(u32 latency_ms, u32 jitter_ms, float loss, u64 seed = 1);
    /// Move the virtual clock; packets become receivable once their time has come.
    void advance(u32 ms);
    u64 now_ms() const { return now_; }
    u32 sent() const { return sent_; }
    u32 dropped() const { return dropped_; }

private:
    struct Packet {
        u64 deliver_at;
        u64 sequence;
        std::vector<u8> bytes;
    };
    class End;

    void enqueue(std::deque<Packet>& queue, const std::vector<u8>& bytes);
    bool dequeue(std::deque<Packet>& queue, std::vector<u8>& out);
    u32 random(u32 bound);

    std::deque<Packet> inbox_a_;
    std::deque<Packet> inbox_b_;
    std::unique_ptr<End> a_;
    std::unique_ptr<End> b_;
    u64 now_ = 0;
    u32 latency_ = 0;
    u32 jitter_ = 0;
    float loss_ = 0;
    u64 rng_ = 0x9E3779B97F4A7C15ull;
    u64 sequence_ = 0;
    u32 sent_ = 0;
    u32 dropped_ = 0;
};

/// N in-process addressed transports with the same fake conditions. End
/// `i` has the address {"mesh", i + 1}; send() goes to its peers, send_to()
/// to any end. For testing lobbies and sessions with more than two players.
class LoopbackMesh {
public:
    explicit LoopbackMesh(u32 ends);
    ~LoopbackMesh();

    AddressedTransport& end(u32 index);
    u32 size() const { return static_cast<u32>(ends_.size()); }
    static Address address_of(u32 index) { return Address{"mesh", static_cast<u16>(index + 1)}; }

    void set_conditions(u32 latency_ms, u32 jitter_ms, float loss, u64 seed = 1);
    void advance(u32 ms);
    u64 now_ms() const { return now_; }

private:
    struct Packet {
        u64 deliver_at;
        u64 sequence;
        Address from;
        std::vector<u8> bytes;
    };
    class End;
    void deliver(u32 to, const Address& from, const std::vector<u8>& bytes);
    bool take(u32 index, std::vector<u8>& out, Address& from);
    u32 random(u32 bound);

    std::vector<std::deque<Packet>> inboxes_;
    std::vector<std::unique_ptr<End>> ends_;
    u64 now_ = 0;
    u32 latency_ = 0;
    u32 jitter_ = 0;
    float loss_ = 0;
    u64 rng_ = 0x9E3779B97F4A7C15ull;
    u64 sequence_ = 0;
};

} // namespace rw::net
