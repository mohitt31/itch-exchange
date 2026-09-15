// Shared vocabulary for every book implementation.
//
// Which ITCH messages touch the book, and which do not:
//
//   A F   add a displayed order
//   E C   execute against a resting order, reducing it
//   X     cancel part of a resting order
//   D     delete a resting order outright
//   U     replace: delete one order, add another. The replacement goes to the
//         back of its level's queue, because it is a new order and time
//         priority is not inherited.
//
//   P Q B do NOT touch the book. P is a non-displayable execution and its
//         order reference is usually zero; Q is a cross; B retracts a trade
//         report. Applying any of them would be a double count against the E
//         and C messages that already moved the book.
#pragma once

#include <cstddef>
#include <vector>

#include "itch/core/hash.hpp"
#include "itch/core/types.hpp"

namespace itch::book {

using itch::Price;
using itch::Qty;
using itch::Side;

// One aggregated price level, for snapshots and cross-implementation compares.
struct LevelSnapshot {
    Price price = 0;
    u64   qty = 0;    // sum of resting quantity at this price
    u32   orders = 0; // number of resting orders at this price

    friend bool operator==(const LevelSnapshot&, const LevelSnapshot&) = default;
};

// Counters every implementation maintains, compared after every operation in
// the differential tests.
struct BookStats {
    u64 total_qty[2] = {0, 0};     // indexed by Side
    u32 level_count[2] = {0, 0};
    u32 order_count = 0;

    friend bool operator==(const BookStats&, const BookStats&) = default;
};

inline constexpr std::size_t side_index(Side s) noexcept {
    return static_cast<std::size_t>(s);
}

// No price is a valid "no best bid" marker, so the empty side is reported
// explicitly instead of through a sentinel that could collide with a real
// price. Callers check empty() first.
inline constexpr Price kNoPrice = 0;

// Feeds a book's full state into a digest in a canonical order: each side,
// levels in price order, and within a level the resting orders in queue order.
// Two implementations that agree on this agree on the book, including time
// priority, not merely on the aggregate depth.
template <class Book>
[[nodiscard]] u64 book_digest(const Book& book) {
    Digest d;
    for (Side s : {Side::Buy, Side::Sell}) {
        d.feed(static_cast<u64>(side_index(s)));
        book.for_each_level(s, [&](Price price, u64 qty, u32 orders, auto&& refs) {
            d.feed(price);
            d.feed(qty);
            d.feed(orders);
            for (OrderRef r : refs) {
                d.feed(r);
            }
        });
        d.feed(0xFFFF'FFFF'FFFF'FFFFULL);  // side terminator
    }
    return d.value();
}

}  // namespace itch::book
