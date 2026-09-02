#pragma once

#include "rewind/core/types.h"
#include "rewind/sim/reflect.h"

#include <type_traits>

namespace rw {

/// PCG32 random number generator. Deterministic, tiny, and trivially
/// copyable so it can live inside the game state and be saved with it.
/// Never call rand() or std::random_device from simulation code; use one of
/// these and store it in your state.
struct Rng {
    u64 state = 0x853c49e6748fea9bull;
    u64 inc = 0xda3e39cb94b95bdbull;

    Rng() = default;
    explicit Rng(u64 seed, u64 sequence = 54) { reseed(seed, sequence); }

    void reseed(u64 seed, u64 sequence = 54) {
        state = 0;
        inc = (sequence << 1u) | 1u;
        next_u32();
        state += seed;
        next_u32();
    }

    u32 next_u32() {
        const u64 old = state;
        state = old * 6364136223846793005ull + inc;
        const u32 xorshifted = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
        const u32 rot = static_cast<u32>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    /// Uniform integer in [0, bound).
    u32 below(u32 bound) { return bound ? next_u32() % bound : 0; }

    /// Uniform integer in [lo, hi], inclusive.
    i32 range(i32 lo, i32 hi) {
        if (hi <= lo) return lo;
        return lo + static_cast<i32>(below(static_cast<u32>(hi - lo + 1)));
    }

    /// Uniform float in [0, 1).
    float unit() { return static_cast<float>(next_u32() >> 8) * (1.0f / 16777216.0f); }

    /// Uniform float in [lo, hi).
    float range_f(float lo, float hi) { return lo + (hi - lo) * unit(); }

    bool chance(float probability) { return unit() < probability; }

    void reflect(Visitor& v) {
        v.field("state", state, {.read_only = true});
        v.field("inc", inc, {.read_only = true});
    }
};

static_assert(std::is_trivially_copyable_v<Rng>);

} // namespace rw
