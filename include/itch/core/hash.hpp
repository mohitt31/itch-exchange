// Deterministic 64-bit digest used by the replay journal.
//
// Fields are fed in explicitly, one call per field. Raw struct bytes are never
// hashed: padding is uninitialised, so hashing it would either trip MSan or,
// worse, produce a digest that looks stable on one build and differs on another.
// The whole point of the digest is to detect exactly that class of bug, so it
// must not contain it.
//
// This is splitmix64's finaliser applied twice per field. It is not a
// cryptographic hash and does not need to be; it needs to be deterministic
// across builds, platforms and optimisation levels, and to have no structured
// collisions on the small integers this project feeds it.
#pragma once

#include <cstddef>
#include <string_view>

#include "itch/core/types.hpp"

namespace itch {

[[nodiscard]] inline constexpr u64 mix64(u64 z) noexcept {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

class Digest {
public:
    constexpr Digest() noexcept = default;

    constexpr void feed(u64 x) noexcept { h_ = mix64(h_ ^ mix64(x)); }
    constexpr void feed_u8(u8 x) noexcept { feed(x); }
    constexpr void feed_u32(u32 x) noexcept { feed(x); }
    constexpr void feed_char(char c) noexcept { feed(static_cast<u8>(c)); }

    constexpr void feed_bytes(const char* p, std::size_t n) noexcept {
        feed(n);
        for (std::size_t i = 0; i < n; ++i) {
            feed(static_cast<u8>(p[i]));
        }
    }

    constexpr void feed_str(std::string_view s) noexcept { feed_bytes(s.data(), s.size()); }

    [[nodiscard]] constexpr u64 value() const noexcept { return h_; }

    constexpr void reset() noexcept { h_ = kSeed; }

private:
    static constexpr u64 kSeed = 0x243f6a8885a308d3ULL;  // pi, first 64 bits
    u64 h_ = kSeed;
};

}  // namespace itch
