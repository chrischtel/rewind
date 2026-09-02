#pragma once

#include "rewind/core/types.h"
#include "rewind/render/sprite.h"
#include "rewind/sim/reflect.h"

#include <algorithm>
#include <string_view>
#include <type_traits>

namespace rw {

/// Playback position inside a SpriteSheet. Plain data on purpose: store it in
/// your game state and it rewinds, replays and hot-reloads with everything
/// else. Time is tracked in whole microseconds so the result never depends on
/// float accumulation order.
struct AnimState {
    u16 tag = 0;
    u16 frame = 0;
    u32 elapsed_us = 0;
    u16 loops = 0;
    u8 backwards = 0;
    u8 finished = 0;

    void reflect(Visitor& v) {
        v.field("tag", tag);
        v.field("frame", frame);
        v.field("elapsed_us", elapsed_us);
        v.field("loops", loops);
        v.field("backwards", backwards, {.min = 0, .max = 1});
        v.field("finished", finished, {.min = 0, .max = 1});
    }
};

static_assert(std::is_trivially_copyable_v<AnimState>);

/// Start playing `tag`. Does nothing if that tag is already playing and has
/// not finished, unless `restart` is set.
inline void anim_play(AnimState& a, const SpriteSheet& sheet, int tag, bool restart = false) {
    if (sheet.tags.empty()) return;
    tag = std::clamp(tag, 0, static_cast<int>(sheet.tags.size()) - 1);
    if (!restart && a.tag == tag && !a.finished) return;
    const SpriteTag& t = sheet.tags[static_cast<usize>(tag)];
    a.tag = static_cast<u16>(tag);
    a.elapsed_us = 0;
    a.loops = 0;
    a.finished = 0;
    a.backwards = (t.direction == TagDirection::Reverse || t.direction == TagDirection::PingPongReverse) ? 1 : 0;
    a.frame = a.backwards ? t.to : t.from;
}

inline bool anim_play(AnimState& a, const SpriteSheet& sheet, std::string_view tag_name, bool restart = false) {
    const int tag = sheet.find_tag(tag_name);
    if (tag < 0) return false;
    anim_play(a, sheet, tag, restart);
    return true;
}

namespace detail {
inline void anim_step(AnimState& a, const SpriteTag& t) {
    const bool ping = t.direction == TagDirection::PingPong || t.direction == TagDirection::PingPongReverse;
    const bool started_backwards =
        t.direction == TagDirection::Reverse || t.direction == TagDirection::PingPongReverse;

    auto complete_loop = [&]() {
        ++a.loops;
        if (t.repeat != 0 && a.loops >= t.repeat) a.finished = 1;
    };

    if (!a.backwards) {
        if (a.frame < t.to) {
            ++a.frame;
        } else if (ping) {
            a.backwards = 1;
            if (started_backwards) complete_loop();
            if (!a.finished) a.frame = t.to > t.from ? static_cast<u16>(t.to - 1) : t.to;
        } else {
            complete_loop();
            if (!a.finished) a.frame = t.from;
        }
    } else {
        if (a.frame > t.from) {
            --a.frame;
        } else if (ping) {
            a.backwards = 0;
            if (!started_backwards) complete_loop();
            if (!a.finished) a.frame = t.to > t.from ? static_cast<u16>(t.from + 1) : t.from;
        } else {
            complete_loop();
            if (!a.finished) a.frame = t.to;
        }
    }
}
} // namespace detail

/// Advance by one fixed step. Frames shorter than the step are skipped over
/// correctly; a finished non-looping animation stays on its last frame.
inline void anim_advance(AnimState& a, const SpriteSheet& sheet, float dt) {
    if (sheet.tags.empty() || sheet.frames.empty() || a.finished) return;
    if (a.tag >= sheet.tags.size()) a.tag = 0;
    const SpriteTag& t = sheet.tags[a.tag];
    if (t.to < t.from || t.to >= sheet.frames.size()) return;
    a.frame = std::clamp(a.frame, t.from, t.to);

    a.elapsed_us += static_cast<u32>(dt * 1000000.0f + 0.5f);
    for (int guard = 0; guard < 1024; ++guard) {
        const u32 duration_us = sheet.frames[a.frame].duration_ms * 1000u;
        if (duration_us == 0 || a.elapsed_us < duration_us) break;
        a.elapsed_us -= duration_us;
        detail::anim_step(a, t);
        if (a.finished) {
            a.elapsed_us = 0;
            break;
        }
    }
}

/// The frame to draw right now.
inline const SpriteFrame& anim_frame(const AnimState& a, const SpriteSheet& sheet) {
    const usize index = std::min<usize>(a.frame, sheet.frames.size() - 1);
    return sheet.frames[index];
}

} // namespace rw
