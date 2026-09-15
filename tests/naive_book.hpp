// A deliberately stupid order book: a flat vector of resting orders, every
// answer recomputed from scratch. Far too slow to use, which is the point --
// it has no incremental state to get wrong, so where a real implementation
// disagrees with it, the real implementation is wrong.
#pragma once

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "itch/book/book_types.hpp"
#include "itch/core/types.hpp"

namespace itch::test {

using itch::OrderRef;
using itch::book::kNoPrice;
using itch::book::Price;
using itch::book::Qty;
using itch::book::Side;

class NaiveBook {
public:
    struct Rec {
        OrderRef ref;
        Side     side;
        Price    price;
        Qty      qty;
    };

    void add(OrderRef ref, Side side, Price price, Qty qty) {
        orders_.push_back(Rec{ref, side, price, qty});
    }

    void cancel(OrderRef ref, Qty shares) {
        Rec* r = find(ref);
        r->qty -= shares;
        if (r->qty == 0) {
            erase(ref);
        }
    }

    void execute(OrderRef ref, Qty shares) { cancel(ref, shares); }

    void remove(OrderRef ref) { cancel(ref, find(ref)->qty); }

    void replace(OrderRef old_ref, OrderRef new_ref, Price price, Qty qty) {
        const Side side = find(old_ref)->side;
        remove(old_ref);
        add(new_ref, side, price, qty);
    }

    [[nodiscard]] bool empty(Side s) const {
        return std::none_of(orders_.begin(), orders_.end(),
                            [&](const Rec& r) { return r.side == s; });
    }

    [[nodiscard]] Price best(Side s) const {
        Price best = kNoPrice;
        bool  any = false;
        for (const Rec& r : orders_) {
            if (r.side != s) {
                continue;
            }
            if (!any || (s == Side::Buy ? r.price > best : r.price < best)) {
                best = r.price;
                any = true;
            }
        }
        return any ? best : kNoPrice;
    }

    [[nodiscard]] u64 qty_at(Side s, Price p) const {
        u64 sum = 0;
        for (const Rec& r : orders_) {
            if (r.side == s && r.price == p) {
                sum += r.qty;
            }
        }
        return sum;
    }

    [[nodiscard]] u32 orders_at(Side s, Price p) const {
        u32 n = 0;
        for (const Rec& r : orders_) {
            if (r.side == s && r.price == p) {
                ++n;
            }
        }
        return n;
    }

    [[nodiscard]] std::size_t order_count() const { return orders_.size(); }

    [[nodiscard]] bool contains(OrderRef ref) const {
        return std::any_of(orders_.begin(), orders_.end(),
                           [&](const Rec& r) { return r.ref == ref; });
    }

    [[nodiscard]] Qty qty_of(OrderRef ref) const { return find(ref)->qty; }
    [[nodiscard]] Side side_of(OrderRef ref) const { return find(ref)->side; }
    [[nodiscard]] Price price_of(OrderRef ref) const { return find(ref)->price; }

    // Levels from the inside out, with the resting order references in the
    // order they arrived, which is what price-time priority means.
    [[nodiscard]] std::vector<std::pair<Price, std::vector<OrderRef>>> levels(Side s) const {
        std::map<Price, std::vector<OrderRef>> by_price;
        for (const Rec& r : orders_) {
            if (r.side == s) {
                by_price[r.price].push_back(r.ref);
            }
        }
        std::vector<std::pair<Price, std::vector<OrderRef>>> out(by_price.begin(),
                                                                 by_price.end());
        if (s == Side::Buy) {
            std::reverse(out.begin(), out.end());
        }
        return out;
    }

    [[nodiscard]] u64 total_qty(Side s) const {
        u64 sum = 0;
        for (const Rec& r : orders_) {
            if (r.side == s) {
                sum += r.qty;
            }
        }
        return sum;
    }

    [[nodiscard]] u32 level_count(Side s) const {
        std::vector<Price> p;
        for (const Rec& r : orders_) {
            if (r.side == s) {
                p.push_back(r.price);
            }
        }
        std::sort(p.begin(), p.end());
        return static_cast<u32>(std::unique(p.begin(), p.end()) - p.begin());
    }

private:
    Rec* find(OrderRef ref) {
        for (Rec& r : orders_) {
            if (r.ref == ref) {
                return &r;
            }
        }
        return nullptr;
    }

    const Rec* find(OrderRef ref) const {
        for (const Rec& r : orders_) {
            if (r.ref == ref) {
                return &r;
            }
        }
        return nullptr;
    }

    void erase(OrderRef ref) {
        orders_.erase(std::remove_if(orders_.begin(), orders_.end(),
                                     [&](const Rec& r) { return r.ref == ref; }),
                      orders_.end());
    }

    std::vector<Rec> orders_;
};

}  // namespace itch::test
