#include "rewind/sim/timeline.h"

#include "rewind/core/profiler.h"

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

void Timeline::fill(Record& record, Game& game, bool keyframe) {
    memory_ -= record.snapshot.size();
    record.snapshot.clear();
    record.hash = 0;
    if (keyframe || cfg_.verify_hashes) {
        scratch_.clear();
        game.save(scratch_);
        record.hash = hash_bytes(scratch_.bytes());
        if (keyframe) {
            record.snapshot = scratch_.bytes();
            memory_ += record.snapshot.size();
        }
    }
}

void Timeline::capture(Game& game, bool force_keyframe) {
    RW_PROFILE_SCOPE("snapshot");
    const bool keyframe = force_keyframe || (head_ % cfg_.keyframe_interval) == 0;
    records_.emplace_back();
    fill(records_.back(), game, keyframe);
}

void Timeline::drop_future() {
    while (newest() > head_) {
        memory_ -= records_.back().snapshot.size();
        records_.pop_back();
    }
}

void Timeline::advance(Game& game, const InputSet& inputs, EventQueue* events) {
    drop_future();
    rec(head_).inputs = inputs;
    {
        RW_PROFILE_SCOPE("tick");
        game.tick(TickContext{head_, dt(), inputs.players[0], events, inputs});
    }
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

void Timeline::resimulate(Game& game, Tick target, bool rewrite) {
    if (records_.empty()) return;
    target = std::clamp(target, head_, newest());

    while (head_ < target) {
        const InputSet& inputs = rec(head_).inputs;
        game.tick(TickContext{head_, dt(), inputs.players[0], nullptr, inputs});
        ++head_;
        Record& r = rec(head_);
        if (rewrite) {
            r.edited = false;
            fill(r, game, (head_ % cfg_.keyframe_interval) == 0 || !r.snapshot.empty());
            continue;
        }
        if (r.edited && !r.snapshot.empty()) {
            // Someone changed the state by hand at this tick. History from
            // here on was recorded from the edited state, so restore it.
            Reader reader(r.snapshot);
            game.load(reader);
            continue;
        }
        if (cfg_.verify_hashes) {
            scratch_.clear();
            game.save(scratch_);
            const u64 actual = hash_bytes(scratch_.bytes());
            const u64 expected = r.hash;
            if (expected != 0 && actual != expected && !desync_) {
                desync_ = DesyncInfo{head_, expected, actual};
            }
        }
    }
}

void Timeline::set_inputs(Tick tick, const InputSet& inputs) {
    if (!in_history(tick) || tick >= newest()) return;
    rec(tick).inputs = inputs;
}

u32 Timeline::rollback(Game& game, Tick from) {
    if (records_.empty() || from >= head_) return 0;
    from = std::max(from, oldest());
    const Tick target = head_;
    seek(game, from);
    const Tick started = head_;
    resimulate(game, target, true);
    return static_cast<u32>(target - started);
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
    fill(r, game, true);
    r.edited = true;
}

usize Timeline::keyframe_count() const {
    usize n = 0;
    for (const Record& r : records_)
        if (!r.snapshot.empty()) ++n;
    return n;
}

} // namespace rw
