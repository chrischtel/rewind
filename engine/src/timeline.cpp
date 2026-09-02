#include "rewind/sim/timeline.h"

#include <algorithm>

namespace rw {

Timeline::Timeline(TimelineConfig cfg) : cfg_(cfg) {
    if (cfg_.keyframe_interval == 0) cfg_.keyframe_interval = 1;
    if (cfg_.tick_rate == 0) cfg_.tick_rate = 60;
}

void Timeline::reset(Game& game) {
    records_.clear();
    memory_ = 0;
    base_ = 0;
    head_ = 0;
    desync_.reset();
    capture(game, true);
}

void Timeline::capture(Game& game, bool force_keyframe) {
    const bool keyframe = force_keyframe || (head_ % cfg_.keyframe_interval) == 0;
    Record record;
    if (keyframe || cfg_.verify_hashes) {
        scratch_.clear();
        game.save(scratch_);
        record.hash = hash_bytes(scratch_.bytes());
        if (keyframe) {
            record.snapshot = scratch_.bytes();
            memory_ += record.snapshot.size();
        }
    }
    records_.push_back(std::move(record));
}

void Timeline::drop_future() {
    while (newest() > head_) {
        memory_ -= records_.back().snapshot.size();
        records_.pop_back();
    }
}

void Timeline::advance(Game& game, const InputFrame& input, EventQueue* events) {
    drop_future();
    rec(head_).input = input;
    game.tick(TickContext{head_, dt(), input, events});
    ++head_;
    capture(game, false);
    trim();
}

void Timeline::trim() {
    const usize limit = static_cast<usize>(cfg_.history_ticks) + 1;
    while (records_.size() > limit) {
        // The oldest record must stay a keyframe, so drop up to the next one.
        usize next = 1;
        while (next < records_.size() && records_[next].snapshot.empty()) ++next;
        if (next >= records_.size()) break;
        for (usize i = 0; i < next; ++i) {
            memory_ -= records_.front().snapshot.size();
            records_.pop_front();
            ++base_;
        }
    }
    if (head_ < base_) head_ = base_;
}

void Timeline::seek(Game& game, Tick target) {
    if (records_.empty()) return;
    target = std::clamp(target, oldest(), newest());
    if (target == head_) return;

    Tick key = target;
    while (rec(key).snapshot.empty()) --key; // base_ is always a keyframe

    Reader reader(rec(key).snapshot);
    game.load(reader);
    head_ = key;
    resimulate(game, target);
}

void Timeline::resimulate(Game& game, Tick target) {
    if (records_.empty()) return;
    target = std::clamp(target, head_, newest());

    while (head_ < target) {
        game.tick(TickContext{head_, dt(), rec(head_).input});
        ++head_;
        if (rec(head_).edited && !rec(head_).snapshot.empty()) {
            // Someone changed the state by hand at this tick. History from
            // here on was recorded from the edited state, so restore it.
            Reader reader(rec(head_).snapshot);
            game.load(reader);
            continue;
        }
        if (cfg_.verify_hashes) {
            scratch_.clear();
            game.save(scratch_);
            const u64 actual = hash_bytes(scratch_.bytes());
            const u64 expected = rec(head_).hash;
            if (expected != 0 && actual != expected && !desync_) {
                desync_ = DesyncInfo{head_, expected, actual};
            }
        }
    }
}

void Timeline::step_forward(Game& game, const InputFrame& live_input) {
    if (head_ < newest())
        resimulate(game, head_ + 1);
    else
        advance(game, live_input);
}

void Timeline::recapture(Game& game) {
    if (records_.empty()) return;
    drop_future();
    Record& r = rec(head_);
    memory_ -= r.snapshot.size();
    scratch_.clear();
    game.save(scratch_);
    r.hash = hash_bytes(scratch_.bytes());
    r.snapshot = scratch_.bytes();
    r.edited = true;
    memory_ += r.snapshot.size();
}

usize Timeline::keyframe_count() const {
    usize n = 0;
    for (const Record& r : records_)
        if (!r.snapshot.empty()) ++n;
    return n;
}

} // namespace rw
