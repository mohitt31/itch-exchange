// Reference limit order book: std::map of price levels, std::list per level.
//
// This is the textbook implementation, and it is here to be a baseline for two
// separate jobs. It is the thing the fast book is benchmarked against, and it
// is an independent implementation to run differential tests against. Those two
// jobs pull in opposite directions -- the honest baseline has to be a fair
// implementation, not a strawman -- so it does the obvious things properly:
//
//   - the order index stores the list iterator, so cancel is O(1) here too.
//     A baseline that scanned the queue would make the comparison meaningless.
//   - the maps are ordered the natural way for each side, so the best price is
//     begin() on both, not begin() on one and rbegin() on the other.
//
// What it does not do is control memory layout, which is the entire point of
// the comparison. Every level is a separate node, every order is a separate
// list node, and the queue for one price is scattered across the heap.
#pragma once

#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "itch/book/book_types.hpp"
#include "itch/core/assert.hpp"
#include "itch/core/types.hpp"

namespace itch::book {

class MapBook {
public:
    struct Level {
        u64                 qty = 0;
        std::list<OrderRef> fifo;
    };

    // Bids descend, asks ascend, so begin() is the inside on both sides.
    using BidMap = std::map<Price, Level, std::greater<Price>>;
    using AskMap = std::map<Price, Level, std::less<Price>>;

    MapBook() = default;

    // --- mutations -------------------------------------------------------

    void add(OrderRef ref, Side side, Price price, Qty qty) {
        ITCH_ASSERT_MSG(qty > 0, "an order with no quantity cannot rest");
        ITCH_ASSERT_MSG(orders_.find(ref) == orders_.end(),
                        "order reference added twice");
        if (side == Side::Buy) {
            insert(bids_, ref, side, price, qty);
        } else {
            insert(asks_, ref, side, price, qty);
        }
    }

    // ITCH order cancel: reduce a resting order by shares. A cancel that takes
    // the order to zero removes it, which does happen even though a full
    // withdrawal normally arrives as a delete.
    void cancel(OrderRef ref, Qty shares) {
        Entry* e = find(ref);
        ITCH_ASSERT_MSG(e != nullptr, "cancel for an order that is not resting");
        ITCH_ASSERT_MSG(shares <= e->qty, "cancel for more than the order holds");
        reduce(*e, ref, shares);
    }

    // ITCH order executed: same effect on the book as a cancel, different
    // meaning. Returns the quantity actually removed.
    Qty execute(OrderRef ref, Qty shares) {
        Entry* e = find(ref);
        ITCH_ASSERT_MSG(e != nullptr, "execution against an order that is not resting");
        ITCH_ASSERT_MSG(shares <= e->qty, "execution for more than the order holds");
        reduce(*e, ref, shares);
        return shares;
    }

    void remove(OrderRef ref) {
        Entry* e = find(ref);
        ITCH_ASSERT_MSG(e != nullptr, "delete for an order that is not resting");
        reduce(*e, ref, e->qty);
    }

    // Delete then add. The replacement is a new order at the back of its queue:
    // time priority is not inherited.
    void replace(OrderRef old_ref, OrderRef new_ref, Price price, Qty qty) {
        const Entry* e = find(old_ref);
        ITCH_ASSERT_MSG(e != nullptr, "replace of an order that is not resting");
        const Side side = e->side;
        remove(old_ref);
        add(new_ref, side, price, qty);
    }

    // --- queries ---------------------------------------------------------

    [[nodiscard]] bool empty(Side s) const noexcept {
        return s == Side::Buy ? bids_.empty() : asks_.empty();
    }

    [[nodiscard]] Price best(Side s) const noexcept {
        if (s == Side::Buy) {
            return bids_.empty() ? kNoPrice : bids_.begin()->first;
        }
        return asks_.empty() ? kNoPrice : asks_.begin()->first;
    }

    [[nodiscard]] u64 qty_at(Side s, Price p) const noexcept {
        if (s == Side::Buy) {
            const auto it = bids_.find(p);
            return it == bids_.end() ? 0 : it->second.qty;
        }
        const auto it = asks_.find(p);
        return it == asks_.end() ? 0 : it->second.qty;
    }

    [[nodiscard]] u32 orders_at(Side s, Price p) const noexcept {
        if (s == Side::Buy) {
            const auto it = bids_.find(p);
            return it == bids_.end() ? 0 : static_cast<u32>(it->second.fifo.size());
        }
        const auto it = asks_.find(p);
        return it == asks_.end() ? 0 : static_cast<u32>(it->second.fifo.size());
    }

    [[nodiscard]] BookStats stats() const noexcept { return stats_; }

    [[nodiscard]] std::size_t order_count() const noexcept { return orders_.size(); }

    // Levels in price order from the inside outwards.
    template <class Fn>
    void for_each_level(Side s, Fn&& fn) const {
        if (s == Side::Buy) {
            for (const auto& [price, lvl] : bids_) {
                fn(price, lvl.qty, static_cast<u32>(lvl.fifo.size()), lvl.fifo);
            }
        } else {
            for (const auto& [price, lvl] : asks_) {
                fn(price, lvl.qty, static_cast<u32>(lvl.fifo.size()), lvl.fifo);
            }
        }
    }

    void snapshot(Side s, std::vector<LevelSnapshot>& out, std::size_t depth = 0) const {
        out.clear();
        for_each_level(s, [&](Price price, u64 qty, u32 orders, const auto&) {
            if (depth == 0 || out.size() < depth) {
                out.push_back(LevelSnapshot{price, qty, orders});
            }
        });
    }

    // --- invariants ------------------------------------------------------

    // O(n) re-derivation of everything the incremental counters claim. Run at
    // invariant level 2, and by the replay driver at intervals below that.
    void validate() const {
        u64 qty[2] = {0, 0};
        u32 levels[2] = {0, 0};
        std::size_t total_orders = 0;

        for (Side s : {Side::Buy, Side::Sell}) {
            Price prev = kNoPrice;
            bool  first = true;
            for_each_level(s, [&](Price price, u64 level_qty, u32 orders, const auto& fifo) {
                ITCH_ASSERT_MSG(orders > 0, "an empty level must not exist");
                ITCH_ASSERT_MSG(fifo.size() == orders, "level order count disagrees");
                if (!first) {
                    // Bids descend, asks ascend, both away from the inside.
                    ITCH_ASSERT_MSG(s == Side::Buy ? price < prev : price > prev,
                                    "levels are out of order");
                }
                first = false;
                prev = price;

                u64 sum = 0;
                for (OrderRef r : fifo) {
                    const auto it = orders_.find(r);
                    ITCH_ASSERT_MSG(it != orders_.end(), "level holds an unknown order");
                    ITCH_ASSERT_MSG(it->second.price == price, "order is on the wrong level");
                    ITCH_ASSERT_MSG(it->second.side == s, "order is on the wrong side");
                    ITCH_ASSERT_MSG(it->second.qty > 0, "a zero quantity order is resting");
                    sum += it->second.qty;
                }
                ITCH_ASSERT_MSG(sum == level_qty, "level quantity disagrees with its orders");

                qty[side_index(s)] += level_qty;
                levels[side_index(s)]++;
                total_orders += orders;
            });
        }

        ITCH_ASSERT_MSG(total_orders == orders_.size(), "orphaned order in the index");
        ITCH_ASSERT_MSG(qty[0] == stats_.total_qty[0] && qty[1] == stats_.total_qty[1],
                        "cached total quantity disagrees");
        ITCH_ASSERT_MSG(levels[0] == stats_.level_count[0] && levels[1] == stats_.level_count[1],
                        "cached level count disagrees");
        ITCH_ASSERT_MSG(stats_.order_count == orders_.size(), "cached order count disagrees");
    }

    // Deliberately not part of validate().
    //
    // "Never crossed" is a property of NASDAQ's displayed feed, not of a limit
    // order book. A book is a container; a matching engine legitimately holds a
    // crossed state for the instant between an aggressing order arriving and
    // the fills being generated, and refusing to represent that would make the
    // structure unusable for the engine that shares it.
    //
    // So the replay driver asserts this against the real feed, where a crossed
    // book means a message was applied wrongly, and the engine does not.
    [[nodiscard]] bool crossed() const noexcept {
        return !bids_.empty() && !asks_.empty() && best(Side::Buy) >= best(Side::Sell);
    }

    [[nodiscard]] bool contains(OrderRef ref) const { return orders_.count(ref) != 0; }

private:
    struct Entry {
        Side                          side;
        Price                         price;
        Qty                           qty;
        std::list<OrderRef>::iterator pos;
    };

    Entry* find(OrderRef ref) {
        const auto it = orders_.find(ref);
        return it == orders_.end() ? nullptr : &it->second;
    }

    const Entry* find(OrderRef ref) const {
        const auto it = orders_.find(ref);
        return it == orders_.end() ? nullptr : &it->second;
    }

    template <class Map>
    void insert(Map& m, OrderRef ref, Side side, Price price, Qty qty) {
        auto [it, fresh] = m.try_emplace(price);
        if (fresh) {
            stats_.level_count[side_index(side)]++;
        }
        it->second.fifo.push_back(ref);
        it->second.qty += qty;
        stats_.total_qty[side_index(side)] += qty;
        stats_.order_count++;
        orders_.emplace(ref, Entry{side, price, qty, std::prev(it->second.fifo.end())});
    }

    void reduce(Entry& e, OrderRef ref, Qty shares) {
        e.qty -= shares;
        stats_.total_qty[side_index(e.side)] -= shares;
        if (e.side == Side::Buy) {
            reduce_level(bids_, e, ref, shares);
        } else {
            reduce_level(asks_, e, ref, shares);
        }
    }

    template <class Map>
    void reduce_level(Map& m, Entry& e, OrderRef ref, Qty shares) {
        const auto it = m.find(e.price);
        ITCH_ASSERT_MSG(it != m.end(), "order points at a level that does not exist");
        it->second.qty -= shares;
        if (e.qty == 0) {
            it->second.fifo.erase(e.pos);
            stats_.order_count--;
            if (it->second.fifo.empty()) {
                ITCH_ASSERT_MSG(it->second.qty == 0, "empty level still holds quantity");
                m.erase(it);
                stats_.level_count[side_index(e.side)]--;
            }
            orders_.erase(ref);
        }
    }

    std::unordered_map<OrderRef, Entry> orders_;
    BidMap                              bids_;
    AskMap                              asks_;
    BookStats                           stats_{};
};

}  // namespace itch::book
