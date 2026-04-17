// SPDX-License-Identifier: LGPL-2.1-or-later

// rng.h — MISSING-11: deterministic, seedable PRNG.
//
// DeterministicRNG is an xoshiro256** generator seeded via splitmix64 from
// a single uint64_t. Given the same seed and the same call sequence, it
// produces identical output across platforms and compilers — which is what
// we need for reproducible geometry decisions (random fuzz factors, mesh
// jittering, tiebreakers in tag allocation, etc).
//
// It is NOT cryptographically secure. Do not use it for anything where
// adversarial prediction matters.
//
// Not thread-safe. Each worker should own its own instance; a shared RNG
// between threads would need external synchronisation and would also lose
// determinism in the process.

#ifndef OREO_RNG_H
#define OREO_RNG_H

#pragma once

#include <cstdint>

namespace oreo {

class DeterministicRNG {
    uint64_t s_[4];

    // splitmix64 — mixes a single seed into a sequence of uncorrelated
    // 64-bit state words. Advancing `state` is explicit so callers can pull
    // multiple values without losing bits.
    static uint64_t splitmix64(uint64_t& state) noexcept {
        uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    static uint64_t rotl(uint64_t x, int k) noexcept {
        return (x << k) | (x >> (64 - k));
    }

public:
    explicit DeterministicRNG(uint64_t seed = 0) noexcept {
        reseed(seed);
    }

    // Re-seed the generator. Deterministic: same seed => same stream.
    void reseed(uint64_t seed) noexcept {
        uint64_t state = seed;
        s_[0] = splitmix64(state);
        s_[1] = splitmix64(state);
        s_[2] = splitmix64(state);
        s_[3] = splitmix64(state);
        // Avoid the all-zero state, which xoshiro cannot escape from.
        // With splitmix64 this is astronomically unlikely but the check is
        // cheap insurance.
        if ((s_[0] | s_[1] | s_[2] | s_[3]) == 0) {
            s_[0] = 0x9E3779B97F4A7C15ULL;
        }
    }

    // Draw the next 64-bit value from the stream (xoshiro256**).
    uint64_t next() noexcept {
        const uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const uint64_t t = s_[1] << 17;

        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];

        s_[2] ^= t;
        s_[3]  = rotl(s_[3], 45);

        return result;
    }

    // Uniform double in [0, 1). 53-bit mantissa resolution.
    double nextUniform() noexcept {
        return (next() >> 11) * (1.0 / (static_cast<uint64_t>(1) << 53));
    }

    // Uniform integer in [lo, hi). Returns lo if lo >= hi — callers that
    // care about the degenerate case should check bounds before calling.
    //
    // The modulo here introduces a vanishingly small bias for extreme
    // ranges; if you need unbiased output across the full 64-bit range,
    // use a rejection-sampling wrapper.
    uint64_t nextRange(uint64_t lo, uint64_t hi) noexcept {
        if (lo >= hi) return lo;
        return lo + (next() % (hi - lo));
    }
};

} // namespace oreo

#endif // OREO_RNG_H
