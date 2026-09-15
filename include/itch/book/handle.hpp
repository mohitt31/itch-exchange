// 32-bit handles: 24-bit slot index, 8-bit generation.
//
// Handles rather than pointers for three reasons. They are half the size, which
// matters because two of them sit in every order record and the record is sized
// to fit four per cache line. They survive the pool being reallocated. And they
// carry a generation, so using one after its slot has been freed is detected
// instead of silently reading whatever now occupies the slot.
//
// Tagged, so an OrderHandle and a LevelHandle are different types. That mixup
// is the one this project would otherwise compile cleanly and get wrong: both
// are u32 indices into a pool, both are in range, and the result is a book that
// looks plausible.
//
// The generation's low bit doubles as the live flag. It is incremented on both
// allocate and free, so an odd generation means the slot is in use and an even
// one means it is free, and a handle to a freed slot fails the compare rather
// than needing a separate liveness lookup on another cache line.
//
// Cost of that: the generation wraps every 128 allocate/free cycles on a slot,
// not 256. A handle held across exactly 128 reuses of its slot will be accepted
// again. This is a bug detector, not a security boundary, and the sanitizer
// build's quarantine free list pushes the practical detection rate to certainty
// for anything a test can produce.
#pragma once

#include <compare>

#include "itch/core/assert.hpp"
#include "itch/core/types.hpp"

namespace itch::book {

template <class Tag>
class Handle {
public:
    static constexpr u32 kIndexBits = 24;
    static constexpr u32 kGenerationBits = 8;
    static constexpr u32 kGenerationMask = (1u << kGenerationBits) - 1;

    // The top index value is reserved for the null handle, so a pool can hold
    // one fewer slot than the index width suggests.
    static constexpr u32 kNullIndex = (1u << kIndexBits) - 1;
    static constexpr u32 kMaxCapacity = kNullIndex;

    constexpr Handle() noexcept = default;

    // Not noexcept: it asserts, and see the note on PriceLadder::best().
    static constexpr Handle make(u32 index, u8 generation) {
        ITCH_ASSERT(index < kMaxCapacity);
        return Handle{(index << kGenerationBits) | generation};
    }

    [[nodiscard]] constexpr u32 index() const noexcept { return bits_ >> kGenerationBits; }
    [[nodiscard]] constexpr u8 generation() const noexcept {
        return static_cast<u8>(bits_ & kGenerationMask);
    }
    [[nodiscard]] constexpr bool is_null() const noexcept { return bits_ == kNullBits; }
    [[nodiscard]] constexpr u32 bits() const noexcept { return bits_; }

    explicit constexpr operator bool() const noexcept { return !is_null(); }

    friend constexpr bool operator==(Handle, Handle) noexcept = default;

private:
    static constexpr u32 kNullBits = 0xFFFF'FFFFu;

    explicit constexpr Handle(u32 bits) noexcept : bits_(bits) {}

    u32 bits_ = kNullBits;
};

struct OrderTag;
struct LevelTag;

using OrderHandle = Handle<OrderTag>;
using LevelHandle = Handle<LevelTag>;

static_assert(sizeof(OrderHandle) == 4, "handles must stay 4 bytes");
static_assert(alignof(OrderHandle) == 4);
static_assert(OrderHandle{}.is_null());
static_assert(!OrderHandle::make(0, 1).is_null());
static_assert(OrderHandle::make(1234, 7).index() == 1234);
static_assert(OrderHandle::make(1234, 7).generation() == 7);
static_assert(OrderHandle::make(OrderHandle::kMaxCapacity - 1, 255).index() ==
              OrderHandle::kMaxCapacity - 1);
// The largest representable non-null handle must not collide with null.
static_assert(!OrderHandle::make(OrderHandle::kMaxCapacity - 1, 255).is_null());

}  // namespace itch::book
