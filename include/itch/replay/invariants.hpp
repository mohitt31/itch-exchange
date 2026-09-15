// Invariant checking and the deterministic journal, as a BookBuilder observer.
//
// The four invariants this project promises to assert throughout a replay:
//
//   never crossed        the displayed feed cannot show a bid at or above an
//                        offer. Unlike the structural checks this is a property
//                        of the feed, so a violation means a message was
//                        applied wrongly, not that the structure is broken.
//   quantity conserved   resting quantity changes by exactly what the messages
//                        said. Tracked independently of the book's own counter
//                        and compared, so the two would have to be wrong in the
//                        same way to agree.
//   no orphaned orders   every indexed order is in a queue and every queued
//                        order is indexed. That is the book's validate().
//   executions match     an execution names an order that is resting. The book
//                        asserts this on every lookup.
//
// The journal digests named fields in a fixed order after every applied
// message. Never raw bytes: padding is uninitialised and a digest containing it
// would differ between builds, which is exactly the failure this is meant to
// detect.
#pragma once

#include "itch/book/book_types.hpp"
#include "itch/core/assert.hpp"
#include "itch/core/hash.hpp"
#include "itch/core/types.hpp"

namespace itch::replay {

using itch::book::Side;
using itch::book::side_index;

struct ReplayCounters {
    u64 applied = 0;
    u64 full_validations = 0;
    u64 crossed_states = 0;
    u64 peak_orders = 0;
    u64 peak_levels = 0;
    u64 expected_qty[2] = {0, 0};
};

class ReplayObserver {
public:
    // validate_every: run the O(n) structural check every N applied messages.
    // 0 disables it. strict: treat a crossed book as an assertion rather than a
    // counter.
    explicit ReplayObserver(u64 validate_every = 0, bool strict = true)
        : validate_every_(validate_every), strict_(strict) {}

    // The builder calls this before an add, while the book still holds the
    // state the message was generated against.
    template <class Book>
    void before_add(const Book&, Side side, Price, Qty qty) {
        counters_.expected_qty[side_index(side)] += qty;
    }

    // Reductions are reported here because the builder knows the quantity and
    // the observer does not see the message.
    void note_reduction(Side side, Qty qty) {
        ITCH_ASSERT_MSG(counters_.expected_qty[side_index(side)] >= qty,
                        "quantity conservation: removed more than was ever added");
        counters_.expected_qty[side_index(side)] -= qty;
    }

    template <class Book>
    void after_apply(const Book& book, Timestamp ts) {
        ++counters_.applied;

        const bool has_bid = !book.empty(Side::Buy);
        const bool has_ask = !book.empty(Side::Sell);
        const Price bid = has_bid ? book.best(Side::Buy) : itch::book::kNoPrice;
        const Price ask = has_ask ? book.best(Side::Sell) : itch::book::kNoPrice;

        if (has_bid && has_ask && bid >= ask) {
            ++counters_.crossed_states;
            ITCH_ASSERT_MSG(!strict_, "book is crossed");
        }

        // Quantity conservation, against a total accumulated from the messages
        // alone. The book's own counter and this one would have to be wrong
        // identically to agree.
        ITCH_INVARIANT_MSG(book.stats().total_qty[0] == counters_.expected_qty[0] &&
                               book.stats().total_qty[1] == counters_.expected_qty[1],
                           "quantity conservation failed");

        counters_.peak_orders =
            (book.order_count() > counters_.peak_orders) ? book.order_count()
                                                         : counters_.peak_orders;
        const u64 levels = book.stats().level_count[0] + book.stats().level_count[1];
        counters_.peak_levels = (levels > counters_.peak_levels) ? levels
                                                                 : counters_.peak_levels;

        // The journal. Named fields, fixed order, no raw bytes.
        digest_.feed(ts);
        digest_.feed(bid);
        digest_.feed(ask);
        digest_.feed(book.stats().total_qty[0]);
        digest_.feed(book.stats().total_qty[1]);
        digest_.feed(book.stats().level_count[0]);
        digest_.feed(book.stats().level_count[1]);
        digest_.feed(book.order_count());

        // A full structural digest covers per-level queue order, and therefore
        // price-time priority, which the aggregates above do not.
        //
        // Folded in on a FIXED interval, deliberately not on validate_every_.
        // The journal has to be a pure function of the input: if a run-time knob
        // could change it, "the digest is identical across builds" would be a
        // statement about the flags rather than about the code. That was found
        // by comparing a release run against a sanitizer run configured with a
        // different validation interval and watching them disagree.
        if (counters_.applied % kStructuralInterval == 0) {
            digest_.feed(itch::book::book_digest(book));
        }

        if (validate_every_ != 0 && counters_.applied % validate_every_ == 0) {
            book.validate();
            ++counters_.full_validations;
        }
    }

    [[nodiscard]] u64 digest() const noexcept { return digest_.value(); }
    [[nodiscard]] const ReplayCounters& counters() const noexcept { return counters_; }

private:
    // Fixed, so the journal is a pure function of the input.
    static constexpr u64 kStructuralInterval = 65536;

    Digest         digest_;
    ReplayCounters counters_{};
    u64            validate_every_ = 0;
    bool           strict_ = true;
};

}  // namespace itch::replay
