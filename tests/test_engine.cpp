// Matching engine.
//
// Every test runs against both FlatBook and MapBook, because the engine is
// templated on the book and a matching bug that only shows on one of them is
// exactly the kind worth catching.

#include <string>
#include <vector>

#include "harness.hpp"
#include "itch/book/flat_book.hpp"
#include "itch/book/map_book.hpp"
#include "itch/engine/matching_engine.hpp"

using namespace itch;
using namespace itch::book;
using namespace itch::engine;

namespace {

constexpr Price kP(int cents) { return static_cast<Price>(100'0000 + cents * 100); }

template <class Book>
struct Fixture {
    Book                     book;
    MatchingEngine<Book>     engine{book};
    std::vector<Fill>        fills;
    OrderRef                 next_maker = 1'000'000;

    // Rests an order without going through the engine, so tests can set up a
    // book without every setup line also being a match.
    OrderRef rest(Side s, Price p, Qty q, u16 owner = 0) {
        const OrderRef r = next_maker++;
        book.add(r, s, p, q, owner);
        return r;
    }

    Report send(Side s, Price p, Qty q, OrderType t = OrderType::Limit,
                TimeInForce tif = TimeInForce::Day, u16 owner = 0) {
        fills.clear();
        NewOrder o;
        o.client_id = client_++;
        o.owner = owner;
        o.side = s;
        o.type = t;
        o.tif = tif;
        o.price = p;
        o.qty = q;
        return engine.submit(o, fills);
    }

    u64 client_ = 1;
};

}  // namespace

// Runs a test body against both book implementations.
#define ENGINE_TEST(name)                                     \
    template <class Book>                                     \
    static void engine_body_##name();                         \
    ITCH_TEST(name) {                                         \
        ITCH_TEST_CONTEXT("book=FlatBook");                   \
        engine_body_##name<FlatBook>();                       \
    }                                                         \
    ITCH_TEST(name##_map) {                                   \
        ITCH_TEST_CONTEXT("book=MapBook");                    \
        engine_body_##name<MapBook>();                        \
    }                                                         \
    template <class Book>                                     \
    static void engine_body_##name()

ENGINE_TEST(engine_no_cross_rests) {
    Fixture<Book> f;
    f.rest(Side::Sell, kP(1), 100);
    const Report r = f.send(Side::Buy, kP(0), 50);
    ITCH_CHECK(r.outcome == Outcome::Rested);
    ITCH_CHECK_EQ(f.fills.size(), std::size_t{0});
    ITCH_CHECK_EQ(r.resting_qty, Qty{50});
    ITCH_CHECK_EQ(f.book.best(Side::Buy), kP(0));
    f.book.validate();
}

ENGINE_TEST(engine_full_fill_against_one_resting_order) {
    Fixture<Book> f;
    const OrderRef maker = f.rest(Side::Sell, kP(1), 100);
    const Report   r = f.send(Side::Buy, kP(1), 100);
    ITCH_CHECK(r.outcome == Outcome::FilledComplete);
    ITCH_REQUIRE_EQ(f.fills.size(), std::size_t{1});
    ITCH_CHECK_EQ(f.fills[0].qty, Qty{100});
    ITCH_CHECK_EQ(f.fills[0].price, kP(1));
    ITCH_CHECK_EQ(f.fills[0].resting, maker);
    ITCH_CHECK(f.book.empty(Side::Sell));
    ITCH_CHECK(f.book.empty(Side::Buy));
    f.book.validate();
}

ENGINE_TEST(engine_partial_fill_leaves_the_maker_resting) {
    Fixture<Book> f;
    f.rest(Side::Sell, kP(1), 100);
    const Report r = f.send(Side::Buy, kP(1), 30);
    ITCH_CHECK(r.outcome == Outcome::FilledComplete);
    ITCH_CHECK_EQ(r.filled, Qty{30});
    ITCH_CHECK_EQ(f.book.qty_at(Side::Sell, kP(1)), u64{70});
    f.book.validate();
}

ENGINE_TEST(engine_aggressor_remainder_rests_at_its_own_price) {
    // It traded at 100.01 but its limit is 100.02, so the remainder rests at
    // 100.02. Resting it at the traded price would quietly give away an
    // improvement the trader never asked for.
    Fixture<Book> f;
    f.rest(Side::Sell, kP(1), 40);
    const Report r = f.send(Side::Buy, kP(2), 100);
    ITCH_CHECK(r.outcome == Outcome::Rested);
    ITCH_CHECK_EQ(r.filled, Qty{40});
    ITCH_CHECK_EQ(r.resting_qty, Qty{60});
    ITCH_CHECK_EQ(f.book.best(Side::Buy), kP(2));
    ITCH_CHECK_EQ(f.book.qty_at(Side::Buy, kP(2)), u64{60});
    f.book.validate();
}

ENGINE_TEST(engine_trade_price_is_the_resting_price) {
    // The buyer was willing to pay 100.05 and the offer was 100.01. The trade
    // happens at 100.01: price improvement goes to whoever crossed the spread.
    Fixture<Book> f;
    f.rest(Side::Sell, kP(1), 100);
    const Report r = f.send(Side::Buy, kP(5), 100);
    ITCH_REQUIRE_EQ(f.fills.size(), std::size_t{1});
    ITCH_CHECK_EQ(f.fills[0].price, kP(1));
    ITCH_CHECK(r.outcome == Outcome::FilledComplete);
}

ENGINE_TEST(engine_sweeps_price_levels_in_order) {
    Fixture<Book> f;
    f.rest(Side::Sell, kP(3), 100);
    f.rest(Side::Sell, kP(1), 100);
    f.rest(Side::Sell, kP(2), 100);

    const Report r = f.send(Side::Buy, kP(3), 250);
    ITCH_CHECK(r.outcome == Outcome::FilledComplete);
    ITCH_REQUIRE_EQ(f.fills.size(), std::size_t{3});
    // Best price first, then outwards.
    ITCH_CHECK_EQ(f.fills[0].price, kP(1));
    ITCH_CHECK_EQ(f.fills[1].price, kP(2));
    ITCH_CHECK_EQ(f.fills[2].price, kP(3));
    ITCH_CHECK_EQ(f.fills[0].qty, Qty{100});
    ITCH_CHECK_EQ(f.fills[1].qty, Qty{100});
    ITCH_CHECK_EQ(f.fills[2].qty, Qty{50});
    ITCH_CHECK_EQ(f.book.qty_at(Side::Sell, kP(3)), u64{50});
    f.book.validate();
}

ENGINE_TEST(engine_time_priority_within_a_level) {
    Fixture<Book> f;
    const OrderRef first = f.rest(Side::Sell, kP(1), 50);
    const OrderRef second = f.rest(Side::Sell, kP(1), 50);
    const OrderRef third = f.rest(Side::Sell, kP(1), 50);

    const Report r = f.send(Side::Buy, kP(1), 120);
    ITCH_CHECK(r.outcome == Outcome::FilledComplete);
    ITCH_REQUIRE_EQ(f.fills.size(), std::size_t{3});
    ITCH_CHECK_EQ(f.fills[0].resting, first);
    ITCH_CHECK_EQ(f.fills[1].resting, second);
    ITCH_CHECK_EQ(f.fills[2].resting, third);
    ITCH_CHECK_EQ(f.fills[2].qty, Qty{20});
    ITCH_CHECK_EQ(f.book.qty_at(Side::Sell, kP(1)), u64{30});
    f.book.validate();
}

ENGINE_TEST(engine_sell_side_matching_mirrors_buy_side) {
    Fixture<Book> f;
    f.rest(Side::Buy, kP(-1), 100);
    f.rest(Side::Buy, kP(-2), 100);
    const Report r = f.send(Side::Sell, kP(-2), 150);
    ITCH_CHECK(r.outcome == Outcome::FilledComplete);
    ITCH_REQUIRE_EQ(f.fills.size(), std::size_t{2});
    // Highest bid first.
    ITCH_CHECK_EQ(f.fills[0].price, kP(-1));
    ITCH_CHECK_EQ(f.fills[1].price, kP(-2));
    f.book.validate();
}

ENGINE_TEST(engine_market_order_takes_whatever_is_there) {
    Fixture<Book> f;
    f.rest(Side::Sell, kP(1), 40);
    f.rest(Side::Sell, kP(9), 40);
    const Report r = f.send(Side::Buy, 0, 100, OrderType::Market);
    ITCH_CHECK(r.outcome == Outcome::Cancelled);  // 80 filled, 20 has nowhere to go
    ITCH_CHECK_EQ(r.filled, Qty{80});
    ITCH_CHECK_EQ(r.resting_ref, OrderRef{0});
    ITCH_CHECK(f.book.empty(Side::Sell));
    ITCH_CHECK(f.book.empty(Side::Buy));  // a market order never rests
    f.book.validate();
}

ENGINE_TEST(engine_market_order_into_an_empty_book) {
    Fixture<Book> f;
    const Report r = f.send(Side::Buy, 0, 100, OrderType::Market);
    ITCH_CHECK(r.outcome == Outcome::Cancelled);
    ITCH_CHECK_EQ(r.filled, Qty{0});
    ITCH_CHECK_EQ(f.fills.size(), std::size_t{0});
}

ENGINE_TEST(engine_ioc_does_not_rest) {
    Fixture<Book> f;
    f.rest(Side::Sell, kP(1), 40);
    const Report r = f.send(Side::Buy, kP(1), 100, OrderType::Limit, TimeInForce::Ioc);
    ITCH_CHECK(r.outcome == Outcome::Cancelled);
    ITCH_CHECK_EQ(r.filled, Qty{40});
    ITCH_CHECK(f.book.empty(Side::Buy));
    f.book.validate();
}

ENGINE_TEST(engine_book_is_never_left_crossed) {
    Fixture<Book> f;
    f.rest(Side::Sell, kP(1), 100);
    f.rest(Side::Sell, kP(2), 100);
    // A buy well through the offers must consume them, not sit above them.
    f.send(Side::Buy, kP(5), 250);
    ITCH_CHECK(!f.book.crossed());
    ITCH_CHECK_EQ(f.book.best(Side::Buy), kP(5));
    ITCH_CHECK(f.book.empty(Side::Sell));
    f.book.validate();
}

// --- self-trade prevention ------------------------------------------------

ENGINE_TEST(engine_stp_off_allows_a_self_trade) {
    Fixture<Book> f;
    f.rest(Side::Sell, kP(1), 100, /*owner=*/7);
    const Report r = f.send(Side::Buy, kP(1), 100, OrderType::Limit, TimeInForce::Day, 7);
    ITCH_CHECK(r.outcome == Outcome::FilledComplete);
    ITCH_CHECK_EQ(f.fills.size(), std::size_t{1});
}

ENGINE_TEST(engine_stp_cancel_newest_rejects_the_aggressor) {
    Fixture<Book> f;
    f.engine.set_stp_mode(StpMode::CancelNewest);
    f.rest(Side::Sell, kP(1), 100, 7);
    const Report r = f.send(Side::Buy, kP(1), 100, OrderType::Limit, TimeInForce::Day, 7);
    ITCH_CHECK(r.outcome == Outcome::StpRejected);
    ITCH_CHECK_EQ(f.fills.size(), std::size_t{0});
    ITCH_CHECK_EQ(f.book.qty_at(Side::Sell, kP(1)), u64{100});  // maker untouched
    ITCH_CHECK(f.book.empty(Side::Buy));
    f.book.validate();
}

ENGINE_TEST(engine_stp_cancel_oldest_pulls_the_maker_and_keeps_going) {
    Fixture<Book> f;
    f.engine.set_stp_mode(StpMode::CancelOldest);
    f.rest(Side::Sell, kP(1), 100, 7);   // same owner: must be pulled
    f.rest(Side::Sell, kP(1), 60, 9);    // different owner: must trade
    const Report r = f.send(Side::Buy, kP(1), 60, OrderType::Limit, TimeInForce::Day, 7);
    ITCH_CHECK(r.outcome == Outcome::FilledComplete);
    ITCH_CHECK_EQ(r.stp_cancelled_resting, u32{1});
    ITCH_REQUIRE_EQ(f.fills.size(), std::size_t{1});
    ITCH_CHECK_EQ(f.fills[0].resting_owner, u16{9});
    ITCH_CHECK(f.book.empty(Side::Sell));
    f.book.validate();
}

ENGINE_TEST(engine_stp_cancel_both_removes_maker_and_stops) {
    Fixture<Book> f;
    f.engine.set_stp_mode(StpMode::CancelBoth);
    f.rest(Side::Sell, kP(1), 100, 7);
    f.rest(Side::Sell, kP(2), 100, 9);
    const Report r = f.send(Side::Buy, kP(2), 150, OrderType::Limit, TimeInForce::Day, 7);
    ITCH_CHECK(r.outcome == Outcome::StpRejected);
    ITCH_CHECK_EQ(r.stp_cancelled_resting, u32{1});
    ITCH_CHECK_EQ(f.fills.size(), std::size_t{0});
    // The same-owner maker is gone; the other one is untouched.
    ITCH_CHECK_EQ(f.book.qty_at(Side::Sell, kP(1)), u64{0});
    ITCH_CHECK_EQ(f.book.qty_at(Side::Sell, kP(2)), u64{100});
    ITCH_CHECK(f.book.empty(Side::Buy));
    f.book.validate();
}

ENGINE_TEST(engine_stp_ignores_the_anonymous_owner) {
    // Owner 0 is "no participant", which every replayed ITCH order carries.
    // Treating those as all-the-same-participant would make STP fire on every
    // match of a replayed book.
    Fixture<Book> f;
    f.engine.set_stp_mode(StpMode::CancelNewest);
    f.rest(Side::Sell, kP(1), 100, 0);
    const Report r = f.send(Side::Buy, kP(1), 100, OrderType::Limit, TimeInForce::Day, 0);
    ITCH_CHECK(r.outcome == Outcome::FilledComplete);
    ITCH_CHECK_EQ(f.fills.size(), std::size_t{1});
}

ENGINE_TEST(engine_misuse_is_detected) {
    Fixture<Book> f;
    ITCH_REQUIRE_ASSERT((void)f.send(Side::Buy, kP(1), 0));   // zero quantity
    ITCH_REQUIRE_ASSERT((void)f.send(Side::Buy, 0, 100));     // limit with no price
}

ENGINE_TEST(engine_conserves_quantity_over_random_flow) {
    constexpr u64 kSeed = 0x5EED0070;
    ITCH_TEST_CONTEXT("seed=" + std::to_string(kSeed));
    itch::test::Rng rng{kSeed};

    Fixture<Book> f;
    f.engine.set_stp_mode(StpMode::CancelNewest);

    u64 sent = 0;
    u64 filled = 0;
    u64 rested = 0;
    u64 cancelled = 0;

    for (int step = 0; step < 20000; ++step) {
        const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
        const bool market = rng.chance(8);
        const auto cents = static_cast<int>(rng.range(0, 20)) - 10;
        const Qty  qty = static_cast<Qty>(rng.range(1, 500));
        const u16  owner = static_cast<u16>(rng.range(1, 4));
        const auto tif = rng.chance(15) ? TimeInForce::Ioc : TimeInForce::Day;

        const Report r =
            f.send(side, market ? 0 : kP(cents), qty,
                   market ? OrderType::Market : OrderType::Limit, tif, owner);
        sent += qty;
        filled += r.filled;
        rested += r.resting_qty;
        if (r.outcome == Outcome::Cancelled || r.outcome == Outcome::StpRejected) {
            cancelled += qty - r.filled;
        }

        // Each fill's quantity is counted once on each side, so the fills have
        // to account for exactly what the report claims was filled.
        Qty from_fills = 0;
        for (const Fill& fl : f.fills) {
            from_fills += fl.qty;
            ITCH_REQUIRE_GT(fl.qty, Qty{0});
            ITCH_REQUIRE_GT(fl.price, Price{0});
        }
        ITCH_REQUIRE_EQ(from_fills, r.filled);

        // A matching engine must never leave the book crossed.
        ITCH_REQUIRE(!f.book.crossed());

        if (step % 211 == 0) {
            ITCH_TEST_CONTEXT("step=" + std::to_string(step));
            f.book.validate();
        }
    }
    f.book.validate();

    // Everything submitted either traded, rested, or was cancelled. Quantity
    // resting on the book plus quantity traded away has to reconcile.
    const u64 on_book = f.book.stats().total_qty[0] + f.book.stats().total_qty[1];
    ITCH_CHECK_EQ(sent, filled + rested + cancelled);
    ITCH_CHECK_GT(f.engine.stats().fills, u64{0});
    ITCH_CHECK_GT(f.engine.stats().stp_events, u64{0});
    ITCH_CHECK_LE(on_book, sent);
}
