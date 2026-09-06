#pragma once

// Undo and redo for tools that edit reflected state: whole-state snapshots
// around each edit streak. The engine's states are small and cheap to
// save, so this stays simple and always correct: undo restores exactly
// what was there, whatever the edit touched.
//
// Feed it once per frame with the current state and whether the previous
// frame edited anything. Consecutive edited frames (a slider drag, a gizmo
// drag) become one entry.

#include "rewind/core/types.h"

#include <vector>

namespace rw {

class EditHistory {
public:
    explicit EditHistory(usize max_entries = 100) : max_entries_(max_entries) {}

    /// Call at the start of every frame, before this frame's edits.
    /// `state` is the state as it is right now; `edited_last_frame` says
    /// whether the previous frame changed it.
    void frame(bool edited_last_frame, std::vector<u8> state) {
        if (edited_last_frame) {
            if (!streak_) {
                streak_ = true;
                pending_before_ = last_state_;
            }
        } else if (streak_) {
            streak_ = false;
            entries_.resize(cursor_); // an edit after undo drops the redo tail
            entries_.push_back(Entry{std::move(pending_before_), state});
            if (entries_.size() > max_entries_) entries_.erase(entries_.begin());
            cursor_ = entries_.size();
        }
        last_state_ = std::move(state);
    }

    bool can_undo() const { return cursor_ > 0; }
    bool can_redo() const { return cursor_ < entries_.size(); }
    usize size() const { return entries_.size(); }
    usize position() const { return cursor_; }

    /// The state to restore, or nullptr. Moves the cursor.
    const std::vector<u8>* undo() {
        if (!can_undo()) return nullptr;
        --cursor_;
        return &entries_[cursor_].before;
    }
    const std::vector<u8>* redo() {
        if (!can_redo()) return nullptr;
        const std::vector<u8>* s = &entries_[cursor_].after;
        ++cursor_;
        return s;
    }

    /// Forget everything (after loading a different scene, say).
    void clear() {
        entries_.clear();
        cursor_ = 0;
        streak_ = false;
    }

private:
    struct Entry {
        std::vector<u8> before;
        std::vector<u8> after;
    };
    usize max_entries_;
    std::vector<Entry> entries_;
    usize cursor_ = 0;
    bool streak_ = false;
    std::vector<u8> last_state_;
    std::vector<u8> pending_before_;
};

} // namespace rw
