// The fast book: pooled records, handle-linked queues, a tick-indexed ladder.
//
// Everything here follows from measurements in DESIGN.md sections 15 to 17.
// The shape, in one paragraph: an order reference is turned into a 32-bit
// handle by an open-addressed index, the handle names a 32-byte record in a
// pool, the record carries a handle to its price level so that cancel never
// consults the ladder at all, and the ladder is only ever touched when a level
// is created or destroyed.
//
// That last point is the one that matters. On this feed adds are 42% of
// messages and deletes 39%, and a delete that does not clear its level touches
// exactly two cache lines: the index entry and the order record.
#pragma once

#include <cstddef>
#include <vector>

#include "itch/book/book_types.hpp"
#include "itch/book/handle.hpp"
#include "itch/book/order_index.hpp"
#include "itch/book/pool.hpp"
#include "itch/book/avl_level_map.hpp"
#include "itch/book/price_ladder.hpp"
#include "itch/book/records.hpp"
#include "itch/core/assert.hpp"
#include "itch/core/types.hpp"

namespace itch::book {

// Templated on the price-to-level structure so that the ladder can be swapped
// for an AVL tree with everything else held constant. That is what makes the
// three-way benchmark measure one variable rather than four at once.
template <class LevelMap, IndexHash HashChoice = IndexHash::Mixed>
class BookT {
public:
    // Defaults come from the measured peaks: 13,843 live orders and 2,057 live
    // levels on the busiest of three symbols over a partial session. Sized
    // generously because the pools are prefaulted once and never grow.
    static constexpr u32 kDefaultOrders = 1u << 18;  // 262,144
    static constexpr u32 kDefaultLevels = 1u << 15;  // 32,768

    explicit BookT(u32 order_capacity = kDefaultOrders,
                   u32 level_capacity = kDefaultLevels)
        : orders_(order_capacity),
          levels_(level_capacity),
          index_(order_capacity),
          ladders_{LevelMap{Side::Buy}, LevelMap{Side::Sell}} {}

    // --- mutations -------------------------------------------------------

    void add(OrderRef ref, Side side, Price price, Qty qty) {
        ITCH_ASSERT_MSG(qty > 0, "an order with no quantity cannot rest");
        ITCH_INVARIANT_MSG(index_.find(ref).is_null(), "order reference added twice");

        const OrderHandle oh = orders_.allocate();
        Order&            o = orders_[oh];
        o.ref = ref;
        o.qty = qty;
        o.price = price;
        o.prev = OrderHandle{};
        o.next = OrderHandle{};
        o.side = static_cast<u8>(side);
        o.flags = 0;

        const LevelHandle lh = level_for(side, price);
        o.level = lh;
        Level& lvl = levels_[lh];

        // Price-time priority: new orders join at the back.
        if (lvl.tail.is_null()) {
            ITCH_INVARIANT(lvl.head.is_null());
            lvl.head = oh;
        } else {
            orders_[lvl.tail].next = oh;
            o.prev = lvl.tail;
        }
        lvl.tail = oh;
        lvl.total_qty += qty;
        ++lvl.order_count;

        index_.insert(ref, oh);
        stats_.total_qty[side_index(side)] += qty;
        ++stats_.order_count;
        note_added(side, price);
    }

    void cancel(OrderRef ref, Qty shares) {
        const OrderHandle oh = require(ref, "cancel for an order that is not resting");
        ITCH_ASSERT_MSG(shares <= orders_[oh].qty, "cancel for more than the order holds");
        reduce(oh, shares);
    }

    Qty execute(OrderRef ref, Qty shares) {
        const OrderHandle oh = require(ref, "execution against an order that is not resting");
        ITCH_ASSERT_MSG(shares <= orders_[oh].qty, "execution for more than the order holds");
        reduce(oh, shares);
        return shares;
    }

    void remove(OrderRef ref) {
        const OrderHandle oh = require(ref, "delete for an order that is not resting");
        reduce(oh, orders_[oh].qty);
    }

    void replace(OrderRef old_ref, OrderRef new_ref, Price price, Qty qty) {
        const OrderHandle oh = require(old_ref, "replace of an order that is not resting");
        const Side        side = static_cast<Side>(orders_[oh].side);
        reduce(oh, orders_[oh].qty);
        add(new_ref, side, price, qty);
    }

    // --- queries ---------------------------------------------------------

    [[nodiscard]] bool empty(Side s) const noexcept {
        return ladders_[side_index(s)].empty();
    }

    // Cached, so this is a load rather than a bitset scan. The cache is
    // recomputed only when the level at the top clears.
    [[nodiscard]] Price best(Side s) const noexcept { return best_[side_index(s)]; }

    [[nodiscard]] u64 qty_at(Side s, Price p) const {
        const LevelHandle lh = ladders_[side_index(s)].find(p);
        return lh.is_null() ? 0 : levels_[lh].total_qty;
    }

    [[nodiscard]] u32 orders_at(Side s, Price p) const {
        const LevelHandle lh = ladders_[side_index(s)].find(p);
        return lh.is_null() ? 0 : levels_[lh].order_count;
    }

    [[nodiscard]] BookStats stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }
    [[nodiscard]] bool contains(OrderRef ref) const { return !index_.find(ref).is_null(); }

    [[nodiscard]] Side side_of(OrderRef ref) const {
        return static_cast<Side>(orders_[require(ref, "side_of for a missing order")].side);
    }
    [[nodiscard]] Price price_of(OrderRef ref) const {
        return orders_[require(ref, "price_of for a missing order")].price;
    }
    [[nodiscard]] Qty qty_of(OrderRef ref) const {
        return orders_[require(ref, "qty_of for a missing order")].qty;
    }

    [[nodiscard]] bool crossed() const noexcept {
        return !empty(Side::Buy) && !empty(Side::Sell) &&
               best(Side::Buy) >= best(Side::Sell);
    }

    // Walks the resting orders of one level in queue order.
    class OrderRange {
    public:
        class Iterator {
        public:
            Iterator(const BookT* book, OrderHandle h) : book_(book), h_(h) {}
            OrderRef operator*() const { return book_->orders_[h_].ref; }
            Iterator& operator++() {
                h_ = book_->orders_[h_].next;
                return *this;
            }
            friend bool operator==(const Iterator& a, const Iterator& b) {
                return a.h_ == b.h_;
            }

        private:
            const BookT* book_;
            OrderHandle  h_;
        };

        OrderRange(const BookT* book, OrderHandle head) : book_(book), head_(head) {}
        [[nodiscard]] Iterator begin() const { return Iterator{book_, head_}; }
        [[nodiscard]] Iterator end() const { return Iterator{book_, OrderHandle{}}; }

    private:
        const BookT* book_;
        OrderHandle  head_;
    };

    // Levels from the inside outwards.
    template <class Fn>
    void for_each_level(Side s, Fn&& fn) const {
        ladders_[side_index(s)].for_each_level([&](Price price, LevelHandle lh) {
            const Level& lvl = levels_[lh];
            fn(price, lvl.total_qty, lvl.order_count, OrderRange{this, lvl.head});
        });
    }

    void snapshot(Side s, std::vector<LevelSnapshot>& out, std::size_t depth = 0) const {
        out.clear();
        for_each_level(s, [&](Price price, u64 qty, u32 orders, const auto&) {
            if (depth == 0 || out.size() < depth) {
                out.push_back(LevelSnapshot{price, qty, orders});
            }
        });
    }

    // --- introspection ---------------------------------------------------

    [[nodiscard]] const LevelMap& ladder(Side s) const { return ladders_[side_index(s)]; }
    [[nodiscard]] const OrderIndex<HashChoice>& index() const { return index_; }
    [[nodiscard]] u32 order_high_water() const noexcept { return orders_.high_water(); }
    [[nodiscard]] u32 level_high_water() const noexcept { return levels_.high_water(); }
    [[nodiscard]] u64 best_recomputes() const noexcept { return best_recomputes_; }

    // --- invariants ------------------------------------------------------

    void validate() const {
        index_.validate();
        orders_.validate();
        levels_.validate();

        u64 qty[2] = {0, 0};
        u32 level_count[2] = {0, 0};
        std::size_t total_orders = 0;

        for (Side s : {Side::Buy, Side::Sell}) {
            ladders_[side_index(s)].validate();
            Price prev = kNoPrice;
            bool  first = true;

            for_each_level(s, [&](Price price, u64 level_qty, u32 orders, const auto& refs) {
                ITCH_ASSERT_MSG(orders > 0, "an empty level must not exist");
                if (!first) {
                    ITCH_ASSERT_MSG(s == Side::Buy ? price < prev : price > prev,
                                    "levels are out of order");
                }
                first = false;
                prev = price;

                u64 sum = 0;
                u32 seen = 0;
                for (OrderRef r : refs) {
                    const OrderHandle oh = index_.find(r);
                    ITCH_ASSERT_MSG(!oh.is_null(), "a queued order is not in the index");
                    const Order& o = orders_[oh];
                    ITCH_ASSERT_MSG(o.price == price, "order is on the wrong level");
                    ITCH_ASSERT_MSG(static_cast<Side>(o.side) == s, "order is on the wrong side");
                    ITCH_ASSERT_MSG(o.qty > 0, "a zero quantity order is resting");
                    sum += o.qty;
                    ++seen;
                }
                ITCH_ASSERT_MSG(seen == orders, "queue length disagrees with the level");
                ITCH_ASSERT_MSG(sum == level_qty, "level quantity disagrees with its orders");

                qty[side_index(s)] += level_qty;
                ++level_count[side_index(s)];
                total_orders += orders;
            });

            // The cached best must equal what the ladder would compute.
            if (!empty(s)) {
                ITCH_ASSERT_MSG(best_[side_index(s)] == ladders_[side_index(s)].best(),
                                "cached best disagrees with the ladder");
            }
        }

        ITCH_ASSERT_MSG(total_orders == index_.size(), "orphaned order in the index");
        ITCH_ASSERT_MSG(qty[0] == stats_.total_qty[0] && qty[1] == stats_.total_qty[1],
                        "cached total quantity disagrees");
        ITCH_ASSERT_MSG(level_count[0] == stats_.level_count[0] &&
                            level_count[1] == stats_.level_count[1],
                        "cached level count disagrees");
        ITCH_ASSERT_MSG(stats_.order_count == index_.size(), "cached order count disagrees");
    }

private:
    [[nodiscard]] OrderHandle require(OrderRef ref, const char* what) const {
        const OrderHandle oh = index_.find(ref);
        ITCH_ASSERT_MSG(!oh.is_null(), what);
        return oh;
    }

    [[nodiscard]] LevelHandle level_for(Side side, Price price) {
        LevelMap& lad = ladders_[side_index(side)];
        const LevelHandle existing = lad.find(price);
        if (!existing.is_null()) {
            return existing;
        }
        const LevelHandle lh = levels_.allocate();
        Level&            lvl = levels_[lh];
        lvl.total_qty = 0;
        lvl.head = OrderHandle{};
        lvl.tail = OrderHandle{};
        lvl.price = price;
        lvl.order_count = 0;
        lad.insert(price, lh);
        ++stats_.level_count[side_index(side)];
        return lh;
    }

    void reduce(OrderHandle oh, Qty shares) {
        Order& o = orders_[oh];
        const Side  side = static_cast<Side>(o.side);
        const Price price = o.price;

        o.qty -= shares;
        stats_.total_qty[side_index(side)] -= shares;
        Level& lvl = levels_[o.level];
        ITCH_INVARIANT_MSG(lvl.total_qty >= shares, "level quantity would go negative");
        lvl.total_qty -= shares;

        if (o.qty != 0) {
            return;
        }

        // Unlink from the queue. O(1) from the middle, which is the whole point
        // of the doubly-linked intrusive list: cancels do not arrive in order.
        if (o.prev.is_null()) {
            lvl.head = o.next;
        } else {
            orders_[o.prev].next = o.next;
        }
        if (o.next.is_null()) {
            lvl.tail = o.prev;
        } else {
            orders_[o.next].prev = o.prev;
        }
        --lvl.order_count;

        index_.erase(o.ref);
        --stats_.order_count;

        const bool level_cleared = lvl.order_count == 0;
        if (level_cleared) {
            ITCH_INVARIANT_MSG(lvl.total_qty == 0, "empty level still holds quantity");
            ITCH_INVARIANT(lvl.head.is_null() && lvl.tail.is_null());
            const LevelHandle lh = o.level;
            ladders_[side_index(side)].erase(price);
            levels_.free(lh);
            --stats_.level_count[side_index(side)];
        }

        // free() overwrites o.next, so nothing may read o after this point.
        orders_.free(oh);

        if (level_cleared) {
            note_removed(side, price);
        }
    }

    void note_added(Side s, Price price) {
        // kNoPrice is zero and no order can rest at zero, so it doubles as the
        // "this side was empty" marker without a separate flag.
        const std::size_t i = side_index(s);
        if (best_[i] == kNoPrice) {
            best_[i] = price;
            return;
        }
        const bool better = (s == Side::Buy) ? price > best_[i] : price < best_[i];
        if (better) {
            best_[i] = price;
        }
    }

    void note_removed(Side s, Price price) {
        const std::size_t i = side_index(s);
        if (price != best_[i]) {
            return;  // a level cleared behind the top; the best is unchanged
        }
        // The top cleared, so the cache has to shift. This is the bitset scan,
        // and it is why the bitset exists.
        ++best_recomputes_;
        best_[i] = ladders_[i].empty() ? kNoPrice : ladders_[i].best();
    }

    Pool<Order, OrderTag>   orders_;
    Pool<Level, LevelTag>   levels_;
    OrderIndex<HashChoice>  index_;
    LevelMap                ladders_[2];
    BookStats               stats_{};
    Price                   best_[2] = {kNoPrice, kNoPrice};
    u64                     best_recomputes_ = 0;
};

// The two implementations that differ only in how a price finds its level.
using FlatBook = BookT<PriceLadder, IndexHash::Mixed>;
using AvlBook = BookT<AvlLevelMap, IndexHash::Mixed>;

// Same structure, identity hashing instead of splitmix64, for the index
// benchmark.
using FlatBookIdentity = BookT<PriceLadder, IndexHash::Identity>;

}  // namespace itch::book
