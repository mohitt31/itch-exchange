// Core scalar types.
//
// Deliberate choice: most quantities are plain integer aliases rather than
// strong types. Strong types are used in exactly one place -- Ticks -- because
// confusing a raw ITCH price with a ladder tick index is the one mixup in this
// codebase that compiles cleanly and produces a silently wrong book. Everywhere
// else the verbosity would buy nothing.
//
// There is no floating point below apps/. ITCH prices are integers with four
// implied decimals and they stay integers through the book, the engine and the
// replay journal. CI greps for float/double in these directories.
#pragma once

#include <compare>
#include <cstdint>

namespace itch {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// Raw ITCH price: USD with four implied decimals, so 1 unit = $0.0001.
// $12.34 is 123400.
using Price = u32;

// Shares.
using Qty = u32;

// NASDAQ order reference number, unique per day per message stream.
using OrderRef = u64;

// Nanoseconds since midnight Eastern. ITCH carries this in 48 bits.
using Timestamp = u64;

inline constexpr Price kPriceScale = 10000;  // raw units per dollar
inline constexpr Price kPennyUnits = 100;    // raw units per $0.01

enum class Side : u8 { Buy = 0, Sell = 1 };

inline constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

inline constexpr char to_char(Side s) noexcept { return s == Side::Buy ? 'B' : 'S'; }

// Ladder index, measured in ticks from the ladder base. Strong type: it must
// never be interchangeable with Price.
struct Ticks {
    i64 v = 0;

    friend constexpr bool operator==(Ticks a, Ticks b) noexcept { return a.v == b.v; }
    friend constexpr auto operator<=>(Ticks a, Ticks b) noexcept { return a.v <=> b.v; }
};

}  // namespace itch
