// The replay journal must be a pure function of the input.
//
// Three separate claims, and they fail for different reasons:
//
//   same input, repeated     -> a difference means hidden state survived
//                               between runs, or something address-dependent
//                               leaked into the digest
//   three implementations    -> a difference means the fast book and the slow
//                               ones disagree about a real day's traffic
//   run-time flags           -> a difference means the digest is a statement
//                               about the flags rather than about the code
//
// The third is here because it was actually broken: the journal folded a full
// structural digest in at --validate-every, so a release run and a sanitizer
// run configured differently produced different digests and looked like a
// miscompilation. The interval is now fixed and this test pins it.

#include <span>
#include <string>
#include <vector>

#include "harness.hpp"
#include "itch/book/avl_level_map.hpp"
#include "itch/book/flat_book.hpp"
#include "itch/book/map_book.hpp"
#include "itch/replay/book_builder.hpp"
#include "itch/replay/invariants.hpp"
#include "itch/wire/framing.hpp"
#include "itch/wire/views.hpp"
#include "wire_builder.hpp"

using namespace itch;
using namespace itch::book;
using namespace itch::wire;
using itch::replay::BookBuilder;
using itch::replay::ReplayObserver;
using itch::test::WireBuilder;

namespace {

// Sanitizer builds run this roughly twenty times slower, and a suite that takes
// five minutes is a suite people stop running. The claims are unchanged -- ten
// runs, three implementations, four validation intervals -- only the session is
// shorter. Every message type and every code path is still exercised; there are
// simply fewer of them.
#if ITCH_INVARIANT_LEVEL >= 2
constexpr int kSessionScale = 5;
#else
constexpr int kSessionScale = 1;
#endif

constexpr int scaled(int n) { return n / kSessionScale; }

// A synthetic ITCH session, so CI can run this without the multi-gigabyte
// corpus. Shaped like the measured feed: mostly adds and deletes, occasional
// stub quotes, and optionally a drifting mid so the ladder has to rebase.
//
// With drift on, this generator CAN produce a crossed book, and that is a
// property of the generator rather than a bug: it does not simulate a book, so
// a bid placed when the mid was high can outlive an ask placed after the mid
// fell. A real exchange would have matched them. So the drifting sessions run
// with the crossed-book assertion off -- they are there to exercise rebasing
// and to pin the digest -- and the never-crossed invariant is asserted on a
// non-drifting session here and, far more meaningfully, against 475,247 real
// operations in apps/itch_replay.
WireBuilder synthetic_session(u64 seed, int messages, bool drift = true) {
    itch::test::Rng rng{seed};
    WireBuilder     b;

    // One stock directory message, so the builder can resolve the symbol.
    {
        const std::size_t m = b.begin('R');
        b.put<u16>(m, offsetof(StockDirectory, stock_locate), 7);
        b.put_alpha(m, offsetof(StockDirectory, stock), 8, "TEST");
    }

    struct Live {
        OrderRef ref;
        Qty      qty;
        bool     buy;
    };
    std::vector<Live> live;
    OrderRef  next = 1;
    Timestamp ts = 34'200'000'000'000ULL;
    i64       centre = 100'0000 / 100;

    for (int i = 0; i < messages; ++i) {
        ts += rng.range(1, 50'000);
        if (drift) {
            centre += static_cast<i64>(rng.range(0, 6)) - 3;
        }
        const u64 roll = rng.range(0, 99);

        if (live.empty() || roll < 50) {
            const bool  buy = rng.chance(50);
            const i64   off = static_cast<i64>(rng.range(1, 40));
            const Price price =
                rng.chance(2) ? (buy ? Price{1} : Price{1'999'999'900})
                              : static_cast<Price>((buy ? centre - off : centre + off) * 100);
            if (price == 0) {
                continue;
            }
            const Qty      qty = static_cast<Qty>(rng.range(1, 900));
            const OrderRef ref = next++;
            const std::size_t m = b.begin('A');
            b.put<u16>(m, offsetof(AddOrder, stock_locate), 7);
            b.put_be48(m, offsetof(AddOrder, timestamp), ts);
            b.put<u64>(m, offsetof(AddOrder, order_reference_number), ref);
            b.put_char(m, offsetof(AddOrder, buy_sell_indicator), buy ? 'B' : 'S');
            b.put<u32>(m, offsetof(AddOrder, shares), qty);
            b.put_alpha(m, offsetof(AddOrder, stock), 8, "TEST");
            b.put<u32>(m, offsetof(AddOrder, price), price);
            live.push_back(Live{ref, qty, buy});
        } else {
            const auto k = static_cast<std::size_t>(rng.range(0, live.size() - 1));
            Live&      o = live[k];
            if (roll < 70) {
                const Qty n = static_cast<Qty>(rng.range(1, o.qty));
                const std::size_t m = b.begin('E');
                b.put<u16>(m, offsetof(OrderExecuted, stock_locate), 7);
                b.put_be48(m, offsetof(OrderExecuted, timestamp), ts);
                b.put<u64>(m, offsetof(OrderExecuted, order_reference_number), o.ref);
                b.put<u32>(m, offsetof(OrderExecuted, executed_shares), n);
                o.qty -= n;
                if (o.qty == 0) {
                    live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
                }
            } else if (roll < 82) {
                const Qty n = static_cast<Qty>(rng.range(1, o.qty));
                const std::size_t m = b.begin('X');
                b.put<u16>(m, offsetof(OrderCancel, stock_locate), 7);
                b.put_be48(m, offsetof(OrderCancel, timestamp), ts);
                b.put<u64>(m, offsetof(OrderCancel, order_reference_number), o.ref);
                b.put<u32>(m, offsetof(OrderCancel, cancelled_shares), n);
                o.qty -= n;
                if (o.qty == 0) {
                    live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
                }
            } else if (roll < 94) {
                const std::size_t m = b.begin('D');
                b.put<u16>(m, offsetof(OrderDelete, stock_locate), 7);
                b.put_be48(m, offsetof(OrderDelete, timestamp), ts);
                b.put<u64>(m, offsetof(OrderDelete, order_reference_number), o.ref);
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
            } else {
                // The replacement keeps the original order's side. Pricing it
                // from the wrong side of the mid is what made this generator
                // produce crossed books.
                const i64   off = static_cast<i64>(rng.range(1, 40));
                const Price price =
                    static_cast<Price>((o.buy ? centre - off : centre + off) * 100);
                if (price == 0) {
                    continue;
                }
                const Qty      qty = static_cast<Qty>(rng.range(1, 900));
                const OrderRef nref = next++;
                const std::size_t m = b.begin('U');
                b.put<u16>(m, offsetof(OrderReplace, stock_locate), 7);
                b.put_be48(m, offsetof(OrderReplace, timestamp), ts);
                b.put<u64>(m, offsetof(OrderReplace, original_order_reference_number), o.ref);
                b.put<u64>(m, offsetof(OrderReplace, new_order_reference_number), nref);
                b.put<u32>(m, offsetof(OrderReplace, shares), qty);
                b.put<u32>(m, offsetof(OrderReplace, price), price);
                o = Live{nref, qty, o.buy};
            }
        }
    }
    return b;
}

template <class Book>
u64 replay_digest(std::span<const std::byte> feed, u64 validate_every, bool strict = false) {
    Book book;
    BookBuilder<Book, ReplayObserver> builder{book, "TEST",
                                              ReplayObserver{validate_every, strict}};
    FrameCursor                cur{feed};
    std::span<const std::byte> f;
    while (cur.next(f) == FrameStatus::Ok) {
        dispatch(f.data(), builder);
    }
    book.validate();
    return builder.observer().digest();
}

}  // namespace

ITCH_TEST(determinism_same_input_same_digest_ten_times) {
    const WireBuilder feed = synthetic_session(0x5EED0060, scaled(40000));
    const u64         first = replay_digest<FlatBook>(feed.span(), 0);
    ITCH_REQUIRE_NE(first, u64{0});
    for (int i = 1; i < 10; ++i) {
        ITCH_TEST_CONTEXT("run=" + std::to_string(i));
        ITCH_REQUIRE_EQ(replay_digest<FlatBook>(feed.span(), 0), first);
    }
}

ITCH_TEST(determinism_three_implementations_agree) {
    const WireBuilder feed = synthetic_session(0x5EED0061, scaled(40000));
    const u64         flat = replay_digest<FlatBook>(feed.span(), 0);
    ITCH_CHECK_EQ(replay_digest<AvlBook>(feed.span(), 0), flat);
    ITCH_CHECK_EQ(replay_digest<MapBook>(feed.span(), 0), flat);
}

ITCH_TEST(determinism_does_not_depend_on_the_validation_interval) {
    // The journal must be a pure function of the input. Folding the structural
    // digest in at --validate-every made it a function of the flags too, and
    // that looked exactly like a miscompilation when a release run was compared
    // against a differently configured sanitizer run.
    const WireBuilder feed = synthetic_session(0x5EED0062, scaled(20000));
    const u64         base = replay_digest<FlatBook>(feed.span(), 0);
    for (u64 every : {u64{1}, u64{7}, u64{999}, u64{50000}}) {
        ITCH_TEST_CONTEXT("validate_every=" + std::to_string(every));
        ITCH_REQUIRE_EQ(replay_digest<FlatBook>(feed.span(), every), base);
    }
}

ITCH_TEST(determinism_digest_reacts_to_a_single_changed_share) {
    // A digest that never changes is not evidence of anything.
    //
    // Note that bumping one add's share count makes the rest of the stream
    // slightly inconsistent -- a later execute sized against the original
    // quantity now leaves a sliver resting. That is fine for this test, which
    // only asks whether the digest notices, and it is why it runs non-strict.
    WireBuilder a = synthetic_session(0x5EED0063, scaled(5000));
    WireBuilder b = synthetic_session(0x5EED0063, scaled(5000));
    ITCH_REQUIRE_EQ(replay_digest<FlatBook>(a.span(), 0), replay_digest<FlatBook>(b.span(), 0));

    // Same stream, one order one share larger.
    WireBuilder c;
    c.append_bytes(a.span());
    // Find the first add and bump its share count.
    FrameCursor                cur{c.span()};
    std::span<const std::byte> f;
    while (cur.next(f) == FrameStatus::Ok) {
        if (static_cast<char>(f[0]) == 'A') {
            const AddOrderView v{f.data()};
            const std::size_t  at = static_cast<std::size_t>(f.data() - c.span().data());
            c.put<u32>(at, offsetof(AddOrder, shares), v.shares() + 1);
            break;
        }
    }
    ITCH_CHECK_NE(replay_digest<FlatBook>(c.span(), 0), replay_digest<FlatBook>(a.span(), 0));
}

ITCH_TEST(determinism_invariants_hold_on_a_synthetic_session) {
    // No drift, so the generated session cannot cross by construction and the
    // strict assertion is meaningful.
    const WireBuilder feed = synthetic_session(0x5EED0064, scaled(60000), /*drift=*/false);
    FlatBook          book;
    BookBuilder<FlatBook, ReplayObserver> builder{book, "TEST", ReplayObserver{1000, true}};
    FrameCursor                cur{feed.span()};
    std::span<const std::byte> f;
    while (cur.next(f) == FrameStatus::Ok) {
        dispatch(f.data(), builder);
    }
    book.validate();
    const auto& c = builder.observer().counters();
    ITCH_CHECK_EQ(c.crossed_states, u64{0});
    ITCH_CHECK_GT(c.applied, u64{1000} / kSessionScale);
    ITCH_CHECK_GT(c.full_validations, u64{0});
    // Quantity conservation, re-derived one more time at the end.
    ITCH_CHECK_EQ(book.stats().total_qty[0], c.expected_qty[0]);
    ITCH_CHECK_EQ(book.stats().total_qty[1], c.expected_qty[1]);
}
