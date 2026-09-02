#pragma once

#include "rewind/core/types.h"

#include <deque>
#include <memory>
#include <vector>

namespace rw::net {

/// Unreliable, unordered datagrams to one peer. UDP in real life, a queue
/// with fake latency in tests. Sessions never assume delivery.
class Transport {
public:
    virtual ~Transport() = default;
    virtual void send(const std::vector<u8>& packet) = 0;
    /// Next packet, if any. Returns false when nothing is waiting.
    virtual bool receive(std::vector<u8>& out) = 0;
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

} // namespace rw::net
