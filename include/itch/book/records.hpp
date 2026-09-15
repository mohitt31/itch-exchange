// The two records the fast book stores, and their layout reasoning.
#pragma once

#include <cstddef>

#include "itch/book/handle.hpp"
#include "itch/core/types.hpp"

namespace itch::book {

// 32 bytes, deliberately.
//
// Four per 128-byte M4 cache line, and a power of two so that turning an index
// into an address is a shift rather than a multiply. 24 bytes would fit five
// per line but costs that multiply on every single access, and 40 would drop to
// three per line.
//
// `ref` could be dropped to reach 24, since the order index already keys on it.
// It stays because it makes "this execution matched an order that existed"
// checkable against the message itself, and because of the shift. Both the
// 32-byte and 24-byte shapes are benchmarked; the number is in NUMBERS.md.
struct Order {
    OrderRef    ref;    //  0   8  the ITCH order reference number
    Qty         qty;    //  8   4  shares still resting
    Price       price;  // 12   4  raw ITCH units
    OrderHandle prev;   // 16   4  queue links, intrusive, within one level
    OrderHandle next;   // 20   4
    LevelHandle level;  // 24   4  back-pointer, so cancel never touches the ladder
    u8          side;   // 28   1
    u8          gen;    // 29   1  pool generation, odd while live
    u16         owner;  // 30   2  participant id; 0 on the replay path, since
                        //         ITCH carries no participant identity. The
                        //         matching engine uses it for self-trade
                        //         prevention, at no cost in size: this field
                        //         was padding either way.

    // While an order sits in the free list it is in no queue, so the forward
    // queue link carries the free list link. Same field, same type, no punning.
    [[nodiscard]] OrderHandle& pool_link() noexcept { return next; }
    [[nodiscard]] u8& pool_generation() noexcept { return gen; }
};

static_assert(sizeof(Order) == 32, "Order must stay 32 bytes: four per cache line");
static_assert(alignof(Order) == 8);
static_assert(offsetof(Order, ref) == 0);
static_assert(offsetof(Order, qty) == 8);
static_assert(offsetof(Order, price) == 12);
static_assert(offsetof(Order, prev) == 16);
static_assert(offsetof(Order, next) == 20);
static_assert(offsetof(Order, level) == 24);

// 32 bytes as well. At peak measured depth (2,057 levels across both sides on
// QQQ) the whole live level pool is 64 KiB and sits inside the P-core's 128 KiB
// L1, which is the entire reason levels are pooled densely instead of living
// inline in the ladder array.
//
// The queue links are in use whenever the level is, so unlike Order this cannot
// borrow one for the free list and carries its own.
struct Level {
    u64         total_qty;    //  0   8  == sum of qty over the queue. Asserted.
    OrderHandle head;         //  8   4  oldest: price-time priority
    OrderHandle tail;         // 12   4
    Price       price;        // 16   4
    u32         order_count;  // 20   4  == queue length. Asserted.
    LevelHandle pool_next;    // 24   4
    u8          gen;          // 28   1
    u8          pad[3];       // 29   3

    [[nodiscard]] LevelHandle& pool_link() noexcept { return pool_next; }
    [[nodiscard]] u8& pool_generation() noexcept { return gen; }
};

static_assert(sizeof(Level) == 32, "Level must stay 32 bytes");
static_assert(alignof(Level) == 8);
static_assert(offsetof(Level, total_qty) == 0);
static_assert(offsetof(Level, head) == 8);
static_assert(offsetof(Level, tail) == 12);
static_assert(offsetof(Level, price) == 16);
static_assert(offsetof(Level, order_count) == 20);

}  // namespace itch::book
