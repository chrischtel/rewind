#pragma once

#include "rewind/core/types.h"
#include "rewind/sim/game.h"

#include <deque>
#include <optional>
#include <vector>

namespace rw {

struct TimelineConfig {
    u32 tick_rate = 60;          ///< Simulation steps per second.
    u32 history_ticks = 60 * 60; ///< How far back you can rewind. Default: one minute.
    u32 keyframe_interval = 1;   ///< Snapshot every N ticks. 1 = instant rewind, most memory.
    bool verify_hashes = true;   ///< Fingerprint every re-simulated tick and flag mismatches.
};

/// A tick whose re-simulation produced a different state than the original run.
struct DesyncInfo {
    Tick tick;
    u64 expected;
    u64 actual;
};

/// The engine's memory of everything that happened.
///
/// For every tick it keeps the inputs that were applied, a hash of the
/// resulting state, and (every `keyframe_interval` ticks) a full snapshot.
/// Seeking to an arbitrary tick loads the nearest earlier snapshot and
/// re-runs the recorded inputs from there, checking each hash on the way.
/// Rollback netcode uses the same machinery: a late remote input corrects a
/// recorded tick, then rollback() re-simulates from there with history
/// rewritten instead of verified.
class Timeline {
public:
    explicit Timeline(TimelineConfig cfg = {});

    /// Forget all history and capture the game's current state as tick 0.
    void reset(Game& game);

    /// Advance from the current head by one tick. If the head is in the past
    /// (after a rewind), everything after it is discarded first: resuming
    /// play from an earlier point creates a new branch of history. Events the
    /// tick emits are appended to `events` when given.
    void advance(Game& game, const InputSet& inputs, EventQueue* events = nullptr);
    void advance(Game& game, const InputFrame& input, EventQueue* events = nullptr) {
        advance(game, InputSet(input), events);
    }

    /// Restore the game to the state it had at `target`. Clamped to history.
    /// Takes the fastest path: loads the nearest snapshot at or before
    /// `target` and only re-simulates the gap. Use this for rewinding.
    void seek(Game& game, Tick target);

    /// Move forward from the current head to `target` by re-running the
    /// recorded inputs through `Game::tick`. With `rewrite` off, every tick's
    /// fingerprint is verified on the way (replay, stepping). With `rewrite`
    /// on, snapshots and fingerprints are replaced instead, which is what a
    /// rollback with corrected inputs needs. `target` is clamped to
    /// [head, newest].
    void resimulate(Game& game, Tick target, bool rewrite = false);

    /// Replace the recorded inputs of a past tick (must be < newest()). The
    /// state from that tick on is now stale; follow with rollback().
    void set_inputs(Tick tick, const InputSet& inputs);

    /// Re-simulate from `from` up to the current head with the inputs now on
    /// record, rewriting history. Returns how many ticks were re-run.
    u32 rollback(Game& game, Tick from);

    /// One tick forward: re-simulate if there is recorded future, otherwise
    /// advance with `live_input`.
    void step_forward(Game& game, const InputFrame& live_input);

    /// The game's state was changed from outside the simulation (inspector,
    /// editor). Snapshot it again as the state of the current tick, discard
    /// the future, and mark the tick as an edit so replays restore the edited
    /// state here instead of reporting a desync.
    void recapture(Game& game);

    Tick head() const { return head_; }
    Tick oldest() const { return base_; }
    Tick newest() const { return base_ + static_cast<Tick>(records_.size()) - 1; }
    bool at_end() const { return head_ == newest(); }
    bool has_history() const { return !records_.empty(); }
    bool in_history(Tick t) const { return has_history() && t >= base_ && t <= newest(); }

    const InputFrame& input_at(Tick t) const { return rec(t).inputs.players[0]; }
    const InputSet& inputs_at(Tick t) const { return rec(t).inputs; }
    u64 hash_at(Tick t) const { return rec(t).hash; }

    u64 memory_bytes() const { return memory_; }
    usize keyframe_count() const;
    const std::optional<DesyncInfo>& last_desync() const { return desync_; }
    void clear_desync() { desync_.reset(); }

    const TimelineConfig& config() const { return cfg_; }
    float dt() const { return 1.0f / static_cast<float>(cfg_.tick_rate); }

private:
    struct Record {
        InputSet inputs{};        ///< Inputs applied to go from this tick to the next.
        u64 hash = 0;             ///< Fingerprint of the state at this tick.
        bool edited = false;      ///< State was replaced by hand here; always has a snapshot.
        std::vector<u8> snapshot; ///< Full state, only on keyframes.
    };

    Record& rec(Tick t) { return records_[static_cast<usize>(t - base_)]; }
    const Record& rec(Tick t) const { return records_[static_cast<usize>(t - base_)]; }

    void fill(Record& record, Game& game, bool keyframe);
    void capture(Game& game, bool force_keyframe);
    void drop_future();
    void trim();

    TimelineConfig cfg_;
    Tick base_ = 0;
    Tick head_ = 0;
    std::deque<Record> records_;
    u64 memory_ = 0;
    std::optional<DesyncInfo> desync_;
    Writer scratch_;
};

} // namespace rw
