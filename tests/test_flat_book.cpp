// Three implementations, one input, byte-identical results.
//
// NaiveBook recomputes everything and has no state to corrupt. MapBook is the
// textbook structure. FlatBook is the one that controls its own memory layout
// and is therefore the one with somewhere to hide a bug. All three are driven
// with the same operations and compared after every step early on, then
// periodically, including per-level queue order -- so price-time priority is
// compared, not just aggregate depth.

#include <string>
#include <vector>

#include "harness.hpp"
#include "itch/book/book_types.hpp"
#include "itch/book/flat_book.hpp"
#include "itch/book/map_book.hpp"
#include "naive_book.hpp"

using namespace itch;
using namespace itch::book;
using itch::test::NaiveBook;

namespace {

// Levels from the inside out, each with its queue in order. The canonical form
// every implementation has to agree on.
template <class Book>
std::vector<std::pair<Price, std::vector<OrderRef>>> layout(const Book& b, Side s) {
    std::vector<std::pair<Price, std::vector<OrderRef>>> out;
    b.for_each_level(s, [&](Price p, u64, u32, const auto& refs) {
        std::vector<OrderRef> q;
        for (OrderRef r : refs) {
            q.push_back(r);
        }
        out.emplace_back(p, std::move(q));
    });
    return out;
}

template <class A, class B>
void same(const A& x, const B& y) {
    ITCH_REQUIRE_EQ(x.order_count(), y.order_count());
    for (Side s : {Side::Buy, Side::Sell}) {
        ITCH_REQUIRE_EQ(x.empty(s), y.empty(s));
        if (!x.empty(s)) {
            ITCH_REQUIRE_EQ(x.best(s), y.best(s));
        }
        ITCH_REQUIRE_EQ(x.stats().total_qty[side_index(s)],
                        y.stats().total_qty[side_index(s)]);
        ITCH_REQUIRE_EQ(x.stats().level_count[side_index(s)],
                        y.stats().level_count[side_index(s)]);

        const auto la = layout(x, s);
        const auto lb = layout(y, s);
        ITCH_REQUIRE_EQ(la.size(), lb.size());
        for (std::size_t i = 0; i < la.size(); ++i) {
            ITCH_REQUIRE_EQ(la[i].first, lb[i].first);
            ITCH_REQUIRE_EQ(la[i].second.size(), lb[i].second.size());
            for (std::size_t j = 0; j < la[i].second.size(); ++j) {
                ITCH_REQUIRE_EQ(la[i].second[j], lb[i].second[j]);
            }
            ITCH_REQUIRE_EQ(x.qty_at(s, la[i].first), y.qty_at(s, la[i].first));
            ITCH_REQUIRE_EQ(x.orders_at(s, la[i].first), y.orders_at(s, la[i].first));
        }
    }
    // One 64-bit number that covers both sides, every level in price order and
    // every order in queue order. If this matches, the books match.
    ITCH_REQUIRE_EQ(book_digest(x), book_digest(y));
}

template <class Book>
void against_naive(const Book& b, const NaiveBook& n) {
    ITCH_REQUIRE_EQ(b.order_count(), n.order_count());
    for (Side s : {Side::Buy, Side::Sell}) {
        ITCH_REQUIRE_EQ(b.empty(s), n.empty(s));
        if (!b.empty(s)) {
            ITCH_REQUIRE_EQ(b.best(s), n.best(s));
        }
        ITCH_REQUIRE_EQ(b.stats().total_qty[side_index(s)], n.total_qty(s));
        ITCH_REQUIRE_EQ(b.stats().level_count[side_index(s)], n.level_count(s));
        const auto la = layout(b, s);
        const auto lb = n.levels(s);
        ITCH_REQUIRE_EQ(la.size(), lb.size());
        for (std::size_t i = 0; i < la.size(); ++i) {
            ITCH_REQUIRE_EQ(la[i].first, lb[i].first);
            ITCH_REQUIRE_EQ(la[i].second.size(), lb[i].second.size());
            for (std::size_t j = 0; j < la[i].second.size(); ++j) {
                ITCH_REQUIRE_EQ(la[i].second[j], lb[i].second[j]);
            }
        }
    }
}

}  // namespace

ITCH_TEST(flat_add_and_query) {
    FlatBook b;
    b.add(1, Side::Buy, 100'0000, 500);
    b.add(2, Side::Buy, 100'0000, 300);
    b.add(3, Side::Buy, 99'9900, 100);
    b.add(4, Side::Sell, 100'0100, 200);

    ITCH_CHECK_EQ(b.best(Side::Buy), Price{100'0000});
    ITCH_CHECK_EQ(b.best(Side::Sell), Price{100'0100});
    ITCH_CHECK_EQ(b.qty_at(Side::Buy, 100'0000), u64{800});
    ITCH_CHECK_EQ(b.orders_at(Side::Buy, 100'0000), u32{2});
    ITCH_CHECK_EQ(b.order_count(), std::size_t{4});
    ITCH_CHECK(!b.crossed());
    b.validate();
}

ITCH_TEST(flat_best_shifts_when_the_top_clears) {
    FlatBook b;
    b.add(1, Side::Buy, 100'0000, 100);
    b.add(2, Side::Buy, 99'9900, 100);
    b.add(3, Side::Buy, 99'9800, 100);
    ITCH_CHECK_EQ(b.best_recomputes(), u64{0});

    b.remove(1);
    ITCH_CHECK_EQ(b.best(Side::Buy), Price{99'9900});
    ITCH_CHECK_EQ(b.best_recomputes(), u64{1});

    // Clearing a level that is not the top must not touch the cache at all.
    b.add(4, Side::Buy, 99'9700, 100);
    b.remove(3);
    ITCH_CHECK_EQ(b.best(Side::Buy), Price{99'9900});
    ITCH_CHECK_EQ(b.best_recomputes(), u64{1});

    b.remove(2);
    b.remove(4);
    ITCH_CHECK(b.empty(Side::Buy));
    ITCH_CHECK_EQ(b.best(Side::Buy), kNoPrice);
    b.validate();
}

ITCH_TEST(flat_cancel_from_the_middle_is_o1_and_keeps_order) {
    FlatBook b;
    b.add(10, Side::Buy, 100'0000, 100);
    b.add(20, Side::Buy, 100'0000, 100);
    b.add(30, Side::Buy, 100'0000, 100);
    b.remove(20);

    const auto l = layout(b, Side::Buy);
    ITCH_REQUIRE_EQ(l.size(), std::size_t{1});
    ITCH_REQUIRE_EQ(l[0].second.size(), std::size_t{2});
    ITCH_CHECK_EQ(l[0].second[0], OrderRef{10});
    ITCH_CHECK_EQ(l[0].second[1], OrderRef{30});
    b.validate();

    // Head and tail removal, which are the two link cases the middle one skips.
    b.remove(10);
    const auto after_head = layout(b, Side::Buy);
    ITCH_CHECK_EQ(after_head[0].second[0], OrderRef{30});
    b.add(40, Side::Buy, 100'0000, 100);
    b.remove(40);
    const auto after_tail = layout(b, Side::Buy);
    ITCH_CHECK_EQ(after_tail[0].second.size(), std::size_t{1});
    b.validate();
}

ITCH_TEST(flat_replace_goes_to_the_back) {
    FlatBook b;
    b.add(10, Side::Buy, 100'0000, 100);
    b.add(20, Side::Buy, 100'0000, 100);
    b.replace(10, 11, 100'0000, 100);
    const auto l = layout(b, Side::Buy);
    ITCH_REQUIRE_EQ(l[0].second.size(), std::size_t{2});
    ITCH_CHECK_EQ(l[0].second[0], OrderRef{20});
    ITCH_CHECK_EQ(l[0].second[1], OrderRef{11});
    ITCH_CHECK(!b.contains(10));
    b.validate();
}

ITCH_TEST(flat_handles_stub_quotes_without_touching_the_window) {
    // The measured reality. These live in the overflow map all session.
    FlatBook b;
    b.add(1, Side::Buy, 100'0000, 100);
    b.add(2, Side::Buy, 1, 100);                 // $0.0001
    b.add(3, Side::Sell, 1'999'999'900, 100);    // $199,999.99
    b.add(4, Side::Sell, 100'0100, 100);

    ITCH_CHECK_EQ(b.best(Side::Buy), Price{100'0000});
    ITCH_CHECK_EQ(b.best(Side::Sell), Price{100'0100});
    ITCH_CHECK_EQ(b.ladder(Side::Buy).overflow_count(), std::size_t{1});
    ITCH_CHECK_EQ(b.ladder(Side::Sell).overflow_count(), std::size_t{1});
    ITCH_CHECK(!b.crossed());
    b.validate();
}

ITCH_TEST(flat_sub_penny_prices_work) {
    FlatBook b;
    b.add(1, Side::Buy, 100'0001, 100);  // $100.0001
    ITCH_CHECK_EQ(b.best(Side::Buy), Price{100'0001});
    ITCH_CHECK_EQ(b.qty_at(Side::Buy, 100'0001), u64{100});
    b.validate();
    b.remove(1);
    ITCH_CHECK(b.empty(Side::Buy));
}

ITCH_TEST(flat_misuse_is_detected) {
    FlatBook b;
    b.add(1, Side::Buy, 100'0000, 100);
    ITCH_REQUIRE_ASSERT(b.add(2, Side::Buy, 100'0000, 0));  // zero quantity
    ITCH_REQUIRE_ASSERT(b.cancel(1, 101));                  // more than it holds
    ITCH_REQUIRE_ASSERT((void)b.execute(999, 1));           // unknown reference
    ITCH_REQUIRE_ASSERT(b.remove(999));
    ITCH_REQUIRE_ASSERT(b.replace(999, 1000, 1, 1));
}

ITCH_TEST(flat_pool_exhaustion_is_an_assertion) {
    FlatBook b{/*orders=*/16, /*levels=*/16};
    for (u64 i = 1; i <= 16; ++i) {
        b.add(i, Side::Buy, 100'0000, 100);
    }
    ITCH_REQUIRE_ASSERT(b.add(17, Side::Buy, 100'0000, 100));
}

ITCH_TEST(flat_matches_map_and_naive_under_random_operations) {
    constexpr u64 kSeed = 0x5EED0040;
    ITCH_TEST_CONTEXT("seed=" + std::to_string(kSeed));
    itch::test::Rng rng{kSeed};

    FlatBook  fast;
    MapBook   ref;
    NaiveBook slow;
    std::vector<OrderRef> live;
    OrderRef next_ref = 1;

    // A drifting mid, so the ladder rebases; a mix of near, far, stub and
    // sub-penny prices, matching the measured shape of the real feed.
    i64 centre = 100'0000 / 100;

    for (int step = 0; step < 40000; ++step) {
        centre += static_cast<i64>(rng.range(0, 6)) - 3;
        const bool can_touch = !live.empty();
        const u64  roll = rng.range(0, 99);

        if (roll < 45 || !can_touch) {
            const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
            Price price;
            const u64 shape = rng.range(0, 999);
            const i64 off = static_cast<i64>(rng.range(1, 30));
            if (shape < 920) {
                price = static_cast<Price>(
                    (side == Side::Buy ? centre - off : centre + off) * 100);
            } else if (shape < 990) {
                const i64 far = static_cast<i64>(rng.range(1, 5000));
                price = static_cast<Price>(
                    (side == Side::Buy ? centre - far : centre + far) * 100);
            } else if (shape < 997) {
                price = side == Side::Buy ? Price{1} : Price{1'999'999'900};
            } else {
                price = static_cast<Price>(
                            (side == Side::Buy ? centre - off : centre + off) * 100) + 1;
            }
            if (price == 0) {
                continue;
            }
            const Qty      qty = static_cast<Qty>(rng.range(1, 1000));
            const OrderRef oref = next_ref++;
            fast.add(oref, side, price, qty);
            ref.add(oref, side, price, qty);
            slow.add(oref, side, price, qty);
            live.push_back(oref);
        } else {
            const auto i = static_cast<std::size_t>(rng.range(0, live.size() - 1));
            const OrderRef r = live[i];
            const Qty      held = slow.qty_of(r);

            if (roll < 65) {
                const Qty n = static_cast<Qty>(rng.range(1, held));
                (void)fast.execute(r, n);
                (void)ref.execute(r, n);
                slow.execute(r, n);
                if (n == held) {
                    live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
                }
            } else if (roll < 80) {
                const Qty n = static_cast<Qty>(rng.range(1, held));
                fast.cancel(r, n);
                ref.cancel(r, n);
                slow.cancel(r, n);
                if (n == held) {
                    live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
                }
            } else if (roll < 92) {
                fast.remove(r);
                ref.remove(r);
                slow.remove(r);
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                const Side side = slow.side_of(r);
                const i64  off = static_cast<i64>(rng.range(1, 30));
                const Price price = static_cast<Price>(
                    (side == Side::Buy ? centre - off : centre + off) * 100);
                if (price == 0) {
                    continue;
                }
                const Qty      qty = static_cast<Qty>(rng.range(1, 1000));
                const OrderRef nref = next_ref++;
                fast.replace(r, nref, price, qty);
                ref.replace(r, nref, price, qty);
                slow.replace(r, nref, price, qty);
                live[i] = nref;
            }
        }

        if (step < 300 || step % 173 == 0) {
            ITCH_TEST_CONTEXT("step=" + std::to_string(step));
            same(fast, ref);
            against_naive(fast, slow);
            fast.validate();
            ref.validate();
        }
    }
    same(fast, ref);
    against_naive(fast, slow);
    fast.validate();

    // The run has to have actually exercised the interesting paths.
    ITCH_CHECK_GT(fast.ladder(Side::Buy).rebases(), u64{0});
    ITCH_CHECK_GT(fast.ladder(Side::Buy).overflow_count() +
                      fast.ladder(Side::Sell).overflow_count(),
                  std::size_t{0});
    ITCH_CHECK_GT(fast.best_recomputes(), u64{0});
    ITCH_CHECK_GT(fast.order_high_water(), u32{0});
}
