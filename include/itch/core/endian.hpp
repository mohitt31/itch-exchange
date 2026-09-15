// Big-endian loads for the ITCH wire format.
//
// The wire structs in wire/messages.hpp exist to pin down and machine-check the
// layout; they are not the parse path. Casting a const std::byte* to a packed
// struct is undefined behaviour on both alignment and aliasing grounds, and
// UBSan reports it correctly. So every field is read here instead, at an offset
// taken from offsetof() on the wire struct. That keeps one source of truth for
// each offset while leaving the reads fully defined.
//
// On arm64 each load_be compiles to an unaligned load plus one rev instruction.
#pragma once

#include <bit>
#include <cstddef>
#include <cstring>
#include <type_traits>

#include "itch/core/types.hpp"

namespace itch {

template <class T>
[[nodiscard]] constexpr T byteswap_uint(T v) noexcept {
    static_assert(std::is_unsigned_v<T>, "byteswap_uint requires an unsigned type");
    if constexpr (sizeof(T) == 1) {
        return v;
    } else if constexpr (sizeof(T) == 2) {
        return static_cast<T>(__builtin_bswap16(v));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(__builtin_bswap32(v));
    } else {
        static_assert(sizeof(T) == 8, "unsupported width");
        return static_cast<T>(__builtin_bswap64(v));
    }
}

// Loads a big-endian unsigned integer of width sizeof(T) from p.
// p need not be aligned. Reads exactly sizeof(T) bytes.
template <class T>
[[nodiscard]] inline T load_be(const std::byte* p) noexcept {
    static_assert(std::is_unsigned_v<T>, "load_be requires an unsigned type");
    T v{};
    std::memcpy(&v, p, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        return byteswap_uint(v);
    } else {
        return v;
    }
}

// ITCH timestamps are 48-bit nanoseconds since midnight. Built from two defined
// loads rather than an 8-byte load at p-2, which would read out of bounds at the
// start of a buffer.
[[nodiscard]] inline u64 load_be48(const std::byte* p) noexcept {
    const u64 hi = load_be<u16>(p);
    const u64 lo = load_be<u32>(p + 2);
    return (hi << 32) | lo;
}

// Used by tests to build wire buffers by hand.
template <class T>
inline void store_be(std::byte* p, T v) noexcept {
    static_assert(std::is_unsigned_v<T>, "store_be requires an unsigned type");
    if constexpr (std::endian::native == std::endian::little) {
        v = byteswap_uint(v);
    }
    std::memcpy(p, &v, sizeof(T));
}

inline void store_be48(std::byte* p, u64 v) noexcept {
    store_be<u16>(p, static_cast<u16>(v >> 32));
    store_be<u32>(p + 2, static_cast<u32>(v));
}

}  // namespace itch
