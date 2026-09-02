#pragma once

#include "rewind/core/types.h"

#include <vector>

namespace rw {

/// Something the simulation wants the outside world to react to: a sound, a
/// screen shake, a particle burst, an achievement. The simulation only emits
/// ids and numbers; what they mean is decided in Game::on_event(), which the
/// engine calls only for ticks that happen live. Re-simulated ticks (rewind,
/// replay, rollback) emit into nothing, so a sound never plays twice.
struct SimEvent {
    u32 id = 0;
    float a = 0;
    float b = 0;
};

struct EventQueue {
    std::vector<SimEvent> items;

    void emit(u32 id, float a = 0, float b = 0) { items.push_back(SimEvent{id, a, b}); }
    void clear() { items.clear(); }
    bool empty() const { return items.empty(); }
};

} // namespace rw
