// MapBook against a deliberately stupid model.
//
// NaiveBook keeps a flat vector of resting orders and recomputes every answer
// from scratch. It is far too slow to use, and that is the point: it has no
// incremental state to get wrong, so where the two disagree, MapBook is wrong.

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "harness.hpp"
#include "itch/book/map_book.hpp"
#include "naive_book.hpp"

using namespace itch;
using namespace itch::book;

namespace {

using itch::test::NaiveBook;

// Compares everything the two can both answer, including per-level queue order.
void compare(const MapBook& fast, const NaiveBook& slow) {
    ITCH_REQUIRE_EQ(fast.order_count(), slow.order_count());
    for (Side s : {Side::Buy, Side::Sell}) {
        ITCH_REQUIRE_EQ(fast.empty(s), slow.empty(s));
        ITCH_REQUIRE_EQ(fast.best(s), slow.best(s));
        ITCH_REQUIRE_EQ(fast.stats().total_qty[side_index(s)], slow.total_qty(s));
        ITCH_REQUIRE_EQ(fast.stats().level_count[side_index(s)], slow.level_count(s));

        const auto expect = slow.levels(s);
        std::vector<std::pair<Price, std::vector<OrderRef>>> got;
        fast.for_each_level(s, [&](Price p, u64, u32, const auto& fifo) {
            got.emplace_back(p, std::vector<OrderRef>(fifo.begin(), fifo.end()));
        });
        ITCH_REQUIRE_EQ(got.size(), expect.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            ITCH_REQUIRE_EQ(got[i].first, expect[i].first);
            ITCH_REQUIRE_EQ(got[i].second.size(), expect[i].second.size());
            for (std::size_t j = 0; j < got[i].second.size(); ++j) {
                ITCH_REQUIRE_EQ(got[i].second[j], expect[i].second[j]);
            }
            ITCH_REQUIRE_EQ(fast.qty_at(s, got[i].first), slow.qty_at(s, got[i].first));
            ITCH_REQUIRE_EQ(fast.orders_at(s, got[i].first), slow.orders_at(s, got[i].first));
        }
    }
    fast.validate();
}

}  // namespace

ITCH_TEST(book_add_and_query) {
    MapBook b;
    b.add(1, Side::Buy, 100'0000, 500);
    b.add(2, Side::Buy, 100'0000, 300);
    b.add(3, Side::Buy, 99'9900, 100);
    b.add(4, Side::Sell, 100'0100, 200);

    ITCH_CHECK_EQ(b.best(Side::Buy), Price{100'0000});
    ITCH_CHECK_EQ(b.best(Side::Sell), Price{100'0100});
    ITCH_CHECK_EQ(b.qty_at(Side::Buy, 100'0000), u64{800});
    ITCH_CHECK_EQ(b.orders_at(Side::Buy, 100'0000), u32{2});
    ITCH_CHECK_EQ(b.stats().total_qty[side_index(Side::Buy)], u64{900});
    ITCH_CHECK_EQ(b.stats().level_count[side_index(Side::Buy)], u32{2});
    ITCH_CHECK_EQ(b.order_count(), std::size_t{4});
    b.validate();
}

ITCH_TEST(book_empty_side_reports_no_price) {
    MapBook b;
    ITCH_CHECK(b.empty(Side::Buy));
    ITCH_CHECK(b.empty(Side::Sell));
    ITCH_CHECK_EQ(b.best(Side::Buy), kNoPrice);
    b.add(1, Side::Buy, 50'0000, 10);
    ITCH_CHECK(!b.empty(Side::Buy));
    ITCH_CHECK(b.empty(Side::Sell));
    b.validate();
}

ITCH_TEST(book_best_shifts_when_a_level_clears) {
    MapBook b;
    b.add(1, Side::Buy, 100'0000, 100);
    b.add(2, Side::Buy, 99'9900, 100);
    b.add(3, Side::Buy, 99'9800, 100);
    ITCH_CHECK_EQ(b.best(Side::Buy), Price{100'0000});

    b.remove(1);
    ITCH_CHECK_EQ(b.best(Side::Buy), Price{99'9900});
    ITCH_CHECK_EQ(b.stats().level_count[side_index(Side::Buy)], u32{2});

    b.remove(2);
    ITCH_CHECK_EQ(b.best(Side::Buy), Price{99'9800});

    b.remove(3);
    ITCH_CHECK(b.empty(Side::Buy));
    ITCH_CHECK_EQ(b.best(Side::Buy), kNoPrice);
    b.validate();
}

ITCH_TEST(book_ask_side_best_is_the_lowest) {
    MapBook b;
    b.add(1, Side::Sell, 100'0300, 100);
    b.add(2, Side::Sell, 100'0100, 100);
    b.add(3, Side::Sell, 100'0200, 100);
    ITCH_CHECK_EQ(b.best(Side::Sell), Price{100'0100});
    b.remove(2);
    ITCH_CHECK_EQ(b.best(Side::Sell), Price{100'0200});
    b.validate();
}

ITCH_TEST(book_partial_execution_keeps_the_order_resting) {
    MapBook b;
    b.add(1, Side::Buy, 100'0000, 500);
    b.execute(1, 200);
    ITCH_CHECK(b.contains(1));
    ITCH_CHECK_EQ(b.qty_at(Side::Buy, 100'0000), u64{300});
    ITCH_CHECK_EQ(b.order_count(), std::size_t{1});

    b.execute(1, 300);
    ITCH_CHECK(!b.contains(1));
    ITCH_CHECK(b.empty(Side::Buy));
    b.validate();
}

ITCH_TEST(book_cancel_to_zero_removes_the_order) {
    MapBook b;
    b.add(1, Side::Buy, 100'0000, 500);
    b.add(2, Side::Buy, 100'0000, 100);
    b.cancel(1, 500);
    ITCH_CHECK(!b.contains(1));
    ITCH_CHECK(b.contains(2));
    ITCH_CHECK_EQ(b.qty_at(Side::Buy, 100'0000), u64{100});
    b.validate();
}

ITCH_TEST(book_queue_is_first_in_first_out) {
    MapBook b;
    b.add(10, Side::Buy, 100'0000, 100);
    b.add(20, Side::Buy, 100'0000, 100);
    b.add(30, Side::Buy, 100'0000, 100);

    std::vector<OrderRef> order;
    b.for_each_level(Side::Buy, [&](Price, u64, u32, const auto& fifo) {
        order.assign(fifo.begin(), fifo.end());
    });
    ITCH_REQUIRE_EQ(order.size(), std::size_t{3});
    ITCH_CHECK_EQ(order[0], OrderRef{10});
    ITCH_CHECK_EQ(order[1], OrderRef{20});
    ITCH_CHECK_EQ(order[2], OrderRef{30});
}

ITCH_TEST(book_cancel_from_the_middle_keeps_the_rest_in_order) {
    MapBook b;
    b.add(10, Side::Buy, 100'0000, 100);
    b.add(20, Side::Buy, 100'0000, 100);
    b.add(30, Side::Buy, 100'0000, 100);
    b.remove(20);

    std::vector<OrderRef> order;
    b.for_each_level(Side::Buy, [&](Price, u64, u32, const auto& fifo) {
        order.assign(fifo.begin(), fifo.end());
    });
    ITCH_REQUIRE_EQ(order.size(), std::size_t{2});
    ITCH_CHECK_EQ(order[0], OrderRef{10});
    ITCH_CHECK_EQ(order[1], OrderRef{30});
    b.validate();
}

ITCH_TEST(book_replace_goes_to_the_back_of_its_queue) {
    // A replaced order is a new order. It does not keep its place in the queue,
    // even when the price is unchanged.
    MapBook b;
    b.add(10, Side::Buy, 100'0000, 100);
    b.add(20, Side::Buy, 100'0000, 100);
    b.replace(10, 11, 100'0000, 100);

    std::vector<OrderRef> order;
    b.for_each_level(Side::Buy, [&](Price, u64, u32, const auto& fifo) {
        order.assign(fifo.begin(), fifo.end());
    });
    ITCH_REQUIRE_EQ(order.size(), std::size_t{2});
    ITCH_CHECK_EQ(order[0], OrderRef{20});
    ITCH_CHECK_EQ(order[1], OrderRef{11});
    ITCH_CHECK(!b.contains(10));
    b.validate();
}

ITCH_TEST(book_replace_keeps_the_side_and_can_move_the_price) {
    MapBook b;
    b.add(10, Side::Sell, 100'0100, 100);
    b.replace(10, 11, 100'0500, 250);
    ITCH_CHECK_EQ(b.best(Side::Sell), Price{100'0500});
    ITCH_CHECK_EQ(b.qty_at(Side::Sell, 100'0500), u64{250});
    ITCH_CHECK(b.empty(Side::Buy));
    b.validate();
}

ITCH_TEST(book_can_hold_a_crossed_state_and_reports_it) {
    // The container must be able to represent this. Only the replay driver
    // treats it as an error, because only the feed promises it cannot happen.
    MapBook b;
    b.add(1, Side::Buy, 100'0200, 100);
    b.add(2, Side::Sell, 100'0100, 100);
    ITCH_CHECK(b.crossed());
    b.validate();

    b.remove(1);
    ITCH_CHECK(!b.crossed());
    b.validate();
}

ITCH_TEST(book_one_sided_is_never_crossed) {
    MapBook b;
    ITCH_CHECK(!b.crossed());
    b.add(1, Side::Buy, 100'0000, 100);
    ITCH_CHECK(!b.crossed());
}

ITCH_TEST(book_touching_prices_are_crossed) {
    // Equal best bid and best offer is a locked book, which the displayed feed
    // also never shows, so it counts as crossed for the replay check.
    MapBook b;
    b.add(1, Side::Buy, 100'0000, 100);
    b.add(2, Side::Sell, 100'0000, 100);
    ITCH_CHECK(b.crossed());
}

ITCH_TEST(book_misuse_is_detected) {
    MapBook b;
    b.add(1, Side::Buy, 100'0000, 100);
    ITCH_REQUIRE_ASSERT(b.add(1, Side::Buy, 100'0000, 100));   // duplicate reference
    ITCH_REQUIRE_ASSERT(b.add(2, Side::Buy, 100'0000, 0));     // zero quantity
    ITCH_REQUIRE_ASSERT(b.cancel(1, 101));                     // more than it holds
    ITCH_REQUIRE_ASSERT(b.execute(999, 1));                    // unknown reference
    ITCH_REQUIRE_ASSERT(b.remove(999));
    ITCH_REQUIRE_ASSERT(b.replace(999, 1000, 1, 1));
}

ITCH_TEST(book_matches_the_naive_model_under_random_operations) {
    constexpr u64 kSeed = 0x5EED0010;
    ITCH_TEST_CONTEXT("seed=" + std::to_string(kSeed));
    itch::test::Rng rng{kSeed};

    MapBook   fast;
    NaiveBook slow;
    std::vector<OrderRef> live;
    OrderRef next_ref = 1;

    // A narrow price band, so levels are created and cleared constantly rather
    // than every order landing on its own price.
    constexpr Price kBase = 100'0000;
    constexpr Price kTick = 100;  // one cent

    for (int step = 0; step < 20000; ++step) {
        const bool can_touch = !live.empty();
        const u64  roll = rng.range(0, 99);

        if (roll < 45 || !can_touch) {
            const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
            // Buys below the mid, sells above, so the generated book stays
            // uncrossed the way a real feed is. The structure would hold a
            // crossed book perfectly well; the point is to keep the model and
            // the book comparable on realistic input.
            const Price price =
                side == Side::Buy ? kBase - static_cast<Price>(rng.range(1, 20)) * kTick
                                  : kBase + static_cast<Price>(rng.range(1, 20)) * kTick;
            const Qty qty = static_cast<Qty>(rng.range(1, 1000));
            const OrderRef ref = next_ref++;
            fast.add(ref, side, price, qty);
            slow.add(ref, side, price, qty);
            live.push_back(ref);
        } else {
            const std::size_t i = static_cast<std::size_t>(rng.range(0, live.size() - 1));
            const OrderRef    ref = live[i];
            const Qty         held = slow.qty_of(ref);

            if (roll < 65) {
                const Qty shares = static_cast<Qty>(rng.range(1, held));
                fast.execute(ref, shares);
                slow.execute(ref, shares);
                if (shares == held) {
                    live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
                }
            } else if (roll < 80) {
                const Qty shares = static_cast<Qty>(rng.range(1, held));
                fast.cancel(ref, shares);
                slow.cancel(ref, shares);
                if (shares == held) {
                    live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
                }
            } else if (roll < 92) {
                fast.remove(ref);
                slow.remove(ref);
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                const Side  side = slow.side_of(ref);
                const Price price =
                    side == Side::Buy ? kBase - static_cast<Price>(rng.range(1, 20)) * kTick
                                      : kBase + static_cast<Price>(rng.range(1, 20)) * kTick;
                const Qty      qty = static_cast<Qty>(rng.range(1, 1000));
                const OrderRef nref = next_ref++;
                fast.replace(ref, nref, price, qty);
                slow.replace(ref, nref, price, qty);
                live[i] = nref;
            }
        }

        // Comparing every step is too slow for 20k steps with an O(n) model,
        // so compare often early and then periodically.
        if (step < 200 || step % 97 == 0) {
            ITCH_TEST_CONTEXT("step=" + std::to_string(step));
            compare(fast, slow);
        }
    }
    compare(fast, slow);
    ITCH_CHECK_GT(fast.order_count(), std::size_t{0});
}
