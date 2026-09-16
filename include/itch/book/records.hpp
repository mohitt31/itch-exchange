// The two records the fast book stores, and their layout reasoning.
#pragma once

#include <cstddef>

#include "itch/book/handle.hpp"
#include "itch/core/types.hpp"

namespace itch::book {

// Unused bytes appended to Order, for the density experiment in DESIGN.md
// section 21. Zero in every real build; a benchmark binary is compiled with a
// non-zero value to measure what four-per-cache-line is actually worth, with
// every other variable held fixed.
#ifndef ITCH_ORDER_PAD_BYTES
#define ITCH_ORDER_PAD_BYTES 0
#endif

// 32 bytes, and the reason is narrower than it first looked.
//
// The original rationale was two claims: a power of two, so turning an index
// into an address is a shift rather than a multiply; and four per 128-byte M4
// cache line. Both were measured (DESIGN.md section 21) and only the first
// survived.
//
//   32 -> 59,447,200 ops/s      40 -> 56,587,736
//   64 -> 59,041,960            56 -> 57,262,185
//
// Power-of-two sizes beat non-power-of-two by 2-4%: that is the shift. But 64
// bytes, at two per cache line instead of four, is as fast as 32. Density does
// not help here because the access pattern is a random single-record lookup
// through a handle, never a scan of adjacent orders -- so a second order on the
// same line is one that will never be read.
//
// 32 is kept because it is a power of two and it is small. The cache-line
// argument was wrong and is recorded as wrong.
//
// `ref` could in principle be dropped to reach 24, but it is load-bearing: the
// reduce path needs it to erase from the order index, and the canonical digest
// needs it to enumerate a level's queue in an implementation-independent way.
// A parallel array would give back the 8 bytes it saved.
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
#if ITCH_ORDER_PAD_BYTES > 0
    u8 experiment_pad[ITCH_ORDER_PAD_BYTES];
#endif

    // While an order sits in the free list it is in no queue, so the forward
    // queue link carries the free list link. Same field, same type, no punning.
    [[nodiscard]] OrderHandle& pool_link() noexcept { return next; }
    [[nodiscard]] u8& pool_generation() noexcept { return gen; }
};

static_assert(sizeof(Order) == 32 + ITCH_ORDER_PAD_BYTES,
              "Order must stay 32 bytes: four per cache line");
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
