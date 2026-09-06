#pragma once

#include "rewind/core/types.h"
#include "rewind/sim/events.h"
#include "rewind/sim/input.h"
#include "rewind/sim/reflect.h"
#include "rewind/sim/serialize.h"

namespace rw {

class App;
class Audio;
class Renderer2D;
class World;
struct AppConfig;

/// Everything a tick is allowed to depend on, plus the one way it may talk
/// back to the outside world.
struct TickContext {
    Tick tick;                      ///< Index of the state being advanced. Starts at 0.
    float dt;                       ///< Fixed step length in seconds, always the same value.
    InputFrame input;               ///< Player 0's input for this tick.
    EventQueue* events = nullptr;   ///< Null while re-simulating; use emit(), never touch directly.
    InputSet inputs{};              ///< Every player's input (multiplayer). inputs[0] == input.

    /// Input of player `i`. Players beyond `inputs.count` read as idle.
    const InputFrame& player(u32 i) const {
        static const InputFrame idle{};
        return i < inputs.count ? inputs.players[i] : idle;
    }
    u32 player_count() const { return inputs.count; }

    /// Announce something for Game::on_event(). Safe to call at any time.
    void emit(u32 id, float a = 0, float b = 0) const {
        if (events) events->emit(id, a, b);
    }
};

/// The contract between a game and the engine.
///
/// The rules that make rewind, replay, hot reload, and later rollback netcode
/// work:
///
///  1. `tick` is the only place simulation state changes, and it may only use
///     what is inside `TickContext` plus the state itself. No clock, no
///     rand(), no reading the keyboard, no static mutable globals.
///  2. The whole simulation state is described by `reflect` (or written by
///     `save` and read by `load`), such that restoring it behaves identically
///     on every future tick.
///  3. `render` reads state but never writes it.
///  4. Side effects (sound, haptics, screen shake) go through `emit()` in
///     tick and are acted on in `on_event`, never inside tick itself.
///
/// Follow those and the engine can scrub through time, verify determinism
/// with a fingerprint per tick, and swap in new code without losing where
/// you are.
class Game {
public:
    virtual ~Game() = default;

    /// Called once before the window exists. Set title, size, tick rate,
    /// history length. Not called again on hot reload.
    virtual void configure(AppConfig&) {}

    /// Called once the App exists, before assets and init. The place to
    /// attach a network session or anything else that needs the App.
    virtual void start(App&) {}

    /// Called once the renderer and audio device exist, and again after every
    /// hot reload. Load textures, sprite sheets and sounds here. Both caches
    /// are keyed by path or name, so repeated calls are cheap. Nothing loaded
    /// here is simulation state; it must not influence tick().
    virtual void load_assets(Renderer2D&, Audio&) {}

    /// Called once before tick 0. Set up the initial state here.
    virtual void init() {}

    /// Advance the simulation by exactly one fixed step.
    virtual void tick(const TickContext& ctx) = 0;

    /// React to an event emitted by a tick that ran live: play a sound, start
    /// a screen shake. Never called for re-simulated ticks.
    virtual void on_event(const SimEvent&, Audio&) {}

    /// Describe the simulation state, one `v.field(...)` per member. This
    /// single description drives saving, loading, the state fingerprint, the
    /// inspector in the overlay, and later the editor and network sync.
    virtual void reflect(Visitor&) {}

    /// Serialize the complete simulation state. The default walks reflect();
    /// override only for hand-tuned formats.
    virtual void save(Writer& out) const {
        SaveVisitor v(out);
        const_cast<Game*>(this)->reflect(v); // SaveVisitor never writes to the fields
    }

    /// Restore state produced by `save`. On hot reload this is how the new
    /// build receives the state of the old one. The default walks reflect().
    virtual void load(Reader& in) {
        LoadVisitor v(in);
        reflect(v);
    }

    /// Draw the current state. `alpha` in [0, 1) is how far real time has moved
    /// toward the next tick, for optional interpolation. It is 0 while paused.
    virtual void render(Renderer2D& renderer, float alpha) = 0;

    /// Optional: draw Dear ImGui widgets inside the engine's debug overlay.
    virtual void debug_ui() {}

    /// The entity world the game keeps, if any. Tools use it for scenes,
    /// prefabs and the entity list. Nullptr means the game has no world.
    virtual World* world() { return nullptr; }
};

} // namespace rw
