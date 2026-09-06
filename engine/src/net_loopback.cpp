#include "rewind/net/transport.h"

#include <algorithm>

namespace rw::net {

// ---- pair ---------------------------------------------------------------------------

/// One end of the pair: sending puts a packet on the *other* end's queue.
class LoopbackPair::End final : public Transport {
public:
    End(LoopbackPair& pair, std::deque<Packet>& inbox, std::deque<Packet>& peer_inbox)
        : pair_(pair), inbox_(inbox), peer_inbox_(peer_inbox) {}

    void send(const std::vector<u8>& packet) override { pair_.enqueue(peer_inbox_, packet); }
    bool receive(std::vector<u8>& out) override { return pair_.dequeue(inbox_, out); }

private:
    LoopbackPair& pair_;
    std::deque<Packet>& inbox_;
    std::deque<Packet>& peer_inbox_;
};

LoopbackPair::LoopbackPair()
    : a_(std::make_unique<End>(*this, inbox_a_, inbox_b_)), b_(std::make_unique<End>(*this, inbox_b_, inbox_a_)) {}

LoopbackPair::~LoopbackPair() = default;

Transport& LoopbackPair::a() { return *a_; }
Transport& LoopbackPair::b() { return *b_; }

void LoopbackPair::set_conditions(u32 latency_ms, u32 jitter_ms, float loss, u64 seed) {
    latency_ = latency_ms;
    jitter_ = jitter_ms;
    loss_ = loss;
    rng_ = seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
}

u32 LoopbackPair::random(u32 bound) {
    // xorshift64*: plenty for shaping fake network conditions.
    rng_ ^= rng_ >> 12;
    rng_ ^= rng_ << 25;
    rng_ ^= rng_ >> 27;
    const u64 v = rng_ * 2685821657736338717ull;
    return bound ? static_cast<u32>((v >> 33) % bound) : 0;
}

void LoopbackPair::advance(u32 ms) { now_ += ms; }

void LoopbackPair::enqueue(std::deque<Packet>& queue, const std::vector<u8>& bytes) {
    ++sent_;
    if (loss_ > 0 && random(10000) < static_cast<u32>(loss_ * 10000.0f)) {
        ++dropped_;
        return;
    }
    Packet p;
    p.deliver_at = now_ + latency_ + (jitter_ ? random(jitter_ + 1) : 0);
    p.sequence = sequence_++;
    p.bytes = bytes;
    queue.push_back(std::move(p));
}

bool LoopbackPair::dequeue(std::deque<Packet>& queue, std::vector<u8>& out) {
    // Deliver the earliest due packet; jitter can reorder, like real UDP.
    auto due = queue.end();
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        if (it->deliver_at > now_) continue;
        if (due == queue.end() || it->deliver_at < due->deliver_at ||
            (it->deliver_at == due->deliver_at && it->sequence < due->sequence))
            due = it;
    }
    if (due == queue.end()) return false;
    out = std::move(due->bytes);
    queue.erase(due);
    return true;
}

// ---- mesh ---------------------------------------------------------------------------

class LoopbackMesh::End final : public AddressedTransport {
public:
    End(LoopbackMesh& mesh, u32 index) : mesh_(mesh), index_(index) {}

    void send(const std::vector<u8>& packet) override {
        for (const Address& p : peers_)
            if (p.port >= 1) mesh_.deliver(p.port - 1, address_of(index_), packet);
    }
    void send_to(const std::vector<u8>& packet, const Address& to) override {
        if (to.port >= 1) mesh_.deliver(to.port - 1, address_of(index_), packet);
    }
    bool receive_from(std::vector<u8>& out, Address& from) override { return mesh_.take(index_, out, from); }
    void add_peer(const Address& peer) override {
        if (std::find(peers_.begin(), peers_.end(), peer) == peers_.end() && peer != address_of(index_)) peers_.push_back(peer);
    }
    void clear_peers() override { peers_.clear(); }
    const std::vector<Address>& peers() const override { return peers_; }

private:
    LoopbackMesh& mesh_;
    u32 index_;
    std::vector<Address> peers_;
};

LoopbackMesh::LoopbackMesh(u32 ends) : inboxes_(ends) {
    for (u32 i = 0; i < ends; ++i) ends_.push_back(std::make_unique<End>(*this, i));
}

LoopbackMesh::~LoopbackMesh() = default;

AddressedTransport& LoopbackMesh::end(u32 index) { return *ends_[index]; }

void LoopbackMesh::set_conditions(u32 latency_ms, u32 jitter_ms, float loss, u64 seed) {
    latency_ = latency_ms;
    jitter_ = jitter_ms;
    loss_ = loss;
    rng_ = seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
}

u32 LoopbackMesh::random(u32 bound) {
    rng_ ^= rng_ >> 12;
    rng_ ^= rng_ << 25;
    rng_ ^= rng_ >> 27;
    const u64 v = rng_ * 2685821657736338717ull;
    return bound ? static_cast<u32>((v >> 33) % bound) : 0;
}

void LoopbackMesh::advance(u32 ms) { now_ += ms; }

void LoopbackMesh::deliver(u32 to, const Address& from, const std::vector<u8>& bytes) {
    if (to >= inboxes_.size()) return;
    if (loss_ > 0 && random(10000) < static_cast<u32>(loss_ * 10000.0f)) return;
    Packet p;
    p.deliver_at = now_ + latency_ + (jitter_ ? random(jitter_ + 1) : 0);
    p.sequence = sequence_++;
    p.from = from;
    p.bytes = bytes;
    inboxes_[to].push_back(std::move(p));
}

bool LoopbackMesh::take(u32 index, std::vector<u8>& out, Address& from) {
    std::deque<Packet>& queue = inboxes_[index];
    auto due = queue.end();
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        if (it->deliver_at > now_) continue;
        if (due == queue.end() || it->deliver_at < due->deliver_at ||
            (it->deliver_at == due->deliver_at && it->sequence < due->sequence))
            due = it;
    }
    if (due == queue.end()) return false;
    out = std::move(due->bytes);
    from = due->from;
    queue.erase(due);
    return true;
}

} // namespace rw::net
