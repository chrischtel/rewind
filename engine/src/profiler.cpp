#include "rewind/core/profiler.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace rw {

Profiler& Profiler::instance() {
    static Profiler profiler;
    return profiler;
}

u64 Profiler::now_ns() const {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void Profiler::begin_frame() {
    current_.clear();
    stack_.clear();
    frame_begin_ns_ = now_ns();
    in_frame_ = true;
}

void Profiler::end_frame() {
    if (!in_frame_) return;
    const u64 end = now_ns();
    // Close anything left open so the last frame is always well formed.
    while (!stack_.empty()) pop();
    last_frame_ns_ = end - frame_begin_ns_;
    last_.swap(current_);
    current_.clear();
    in_frame_ = false;

    history_[history_pos_] = static_cast<float>(last_frame_ns_) / 1e6f;
    history_pos_ = (history_pos_ + 1) % kHistory;
    if (history_count_ < kHistory) ++history_count_;
}

void Profiler::push(const char* name) {
    if (!enabled_ || !in_frame_) return;
    ProfileSample s{};
    std::strncpy(s.name, name ? name : "?", sizeof s.name - 1);
    s.depth = static_cast<int>(stack_.size());
    s.begin_ns = now_ns() - frame_begin_ns_;
    s.end_ns = s.begin_ns;
    stack_.push_back(current_.size());
    current_.push_back(s);
}

void Profiler::pop() {
    if (!enabled_ || !in_frame_ || stack_.empty()) return;
    current_[stack_.back()].end_ns = now_ns() - frame_begin_ns_;
    stack_.pop_back();
}

void Profiler::history(std::vector<float>& out) const {
    out.clear();
    out.reserve(history_count_);
    const usize start = history_count_ < kHistory ? 0 : history_pos_;
    for (usize i = 0; i < history_count_; ++i) out.push_back(history_[(start + i) % kHistory]);
}

float Profiler::average_ms() const {
    if (history_count_ == 0) return 0;
    float sum = 0;
    for (usize i = 0; i < history_count_; ++i) sum += history_[i];
    return sum / static_cast<float>(history_count_);
}

float Profiler::worst_ms() const {
    float worst = 0;
    for (usize i = 0; i < history_count_; ++i) worst = std::max(worst, history_[i]);
    return worst;
}

} // namespace rw
