// Price-time priority matching over the same book the replay path uses.
//
// The two paths are different in kind. ITCH reports matches that NASDAQ already
// made; this decides them. What they share is the data structure, which is the
// point -- the layout work in sections 15 to 18 was not book-building work, it
// was order book work, and an engine is the other thing an order book is for.
//
// Scope, matching the project's cut list: limit and market orders, partial
// fills, resting versus aggressing, self-trade prevention. No stops, no pegs,
// no auctions, no icebergs, no threading.
//
// Two rules that are easy to get subtly wrong and are pinned by tests:
//
//   trade price is the RESTING order's price, not the aggressor's. Price
//   improvement belongs to the side that arrived later and crossed the spread.
//
//   an aggressing order that does not fully fill rests at ITS OWN limit price,
//   not at the price it traded at.
#pragma once

#include <cstddef>
#include <vector>

#include "itch/book/book_types.hpp"
#include "itch/core/assert.hpp"
#include "itch/core/types.hpp"

namespace itch::engine {

using itch::book::Side;
using itch::book::kNoPrice;

enum class OrderType : u8 {
    Limit,
    Market,
};

enum class TimeInForce : u8 {
    Day,  // rest whatever does not fill
    Ioc,  // cancel whatever does not fill immediately
};

// What to do when an incoming order would trade against a resting order that
// belongs to the same participant. Every venue offers some form of this; the
// three below are the common ones.
enum class StpMode : u8 {
    Off,
    CancelNewest,  // reject the incoming order's remainder
    CancelOldest,  // pull the resting order, keep matching
    CancelBoth,    // pull both
};

struct NewOrder {
    u64         client_id = 0;
    u16         owner = 0;
    Side        side = Side::Buy;
    OrderType   type = OrderType::Limit;
    TimeInForce tif = TimeInForce::Day;
    Price       price = 0;  // ignored for Market
    Qty         qty = 0;
};

struct Fill {
    u64   aggressor = 0;
    u64   resting = 0;
    u16   aggressor_owner = 0;
    u16   resting_owner = 0;
    Price price = 0;
    Qty   qty = 0;
};

enum class Outcome : u8 {
    Rested,          // some or all of it is on the book
    FilledComplete,  // fully filled, nothing rested
    Cancelled,       // nothing rested: IOC remainder, or market with no book
    StpRejected,     // stopped by self-trade prevention
};

struct Report {
    Outcome  outcome = Outcome::Cancelled;
    OrderRef resting_ref = 0;  // nonzero if any quantity rested
    Qty      filled = 0;
    Qty      resting_qty = 0;
    u32      stp_cancelled_resting = 0;
};

struct EngineStats {
    u64 submitted = 0;
    u64 fills = 0;
    u64 filled_qty = 0;
    u64 rested = 0;
    u64 cancelled = 0;
    u64 stp_events = 0;
};

template <class Book>
class MatchingEngine {
public:
    explicit MatchingEngine(Book& book, StpMode stp = StpMode::Off)
        : book_(book), stp_(stp) {}

    // Order references the engine assigns are dense and start at 1, so 0 is
    // free to mean "nothing rested".
    Report submit(const NewOrder& order, std::vector<Fill>& fills) {
        ITCH_ASSERT_MSG(order.qty > 0, "an order with no quantity cannot be submitted");
        ITCH_ASSERT_MSG(order.type == OrderType::Market || order.price > 0,
                        "a limit order needs a price");
        ++stats_.submitted;

        Report     report;
        const Side other = itch::opposite(order.side);
        Qty        remaining = order.qty;
        bool       stp_stop = false;

        while (remaining > 0 && !book_.empty(other)) {
            const Price touch = book_.best(other);
            if (!crosses(order, touch)) {
                break;
            }

            const OrderRef resting = book_.front_at(other, touch);
            ITCH_ASSERT_MSG(resting != 0, "the best level has no resting order");

            if (stp_ != StpMode::Off && book_.owner_of(resting) == order.owner &&
                order.owner != 0) {
                ++stats_.stp_events;
                switch (stp_) {
                    case StpMode::CancelOldest:
                        book_.remove(resting);
                        ++report.stp_cancelled_resting;
                        continue;  // the aggressor lives on
                    case StpMode::CancelBoth:
                        book_.remove(resting);
                        ++report.stp_cancelled_resting;
                        stp_stop = true;
                        break;
                    case StpMode::CancelNewest:
                        stp_stop = true;
                        break;
                    case StpMode::Off:
                        break;
                }
                if (stp_stop) {
                    break;
                }
            }

            const Qty available = book_.qty_of(resting);
            const Qty traded = (remaining < available) ? remaining : available;

            // The resting order's price. Price improvement goes to whoever
            // crossed the spread, which is the aggressor.
            fills.push_back(Fill{order.client_id, resting, order.owner,
                                 book_.owner_of(resting), touch, traded});
            ++stats_.fills;
            stats_.filled_qty += traded;

            (void)book_.execute(resting, traded);
            remaining -= traded;
            report.filled += traded;
        }

        if (stp_stop) {
            report.outcome = Outcome::StpRejected;
            ++stats_.cancelled;
            return report;
        }
        if (remaining == 0) {
            report.outcome = Outcome::FilledComplete;
            return report;
        }
        // A market order never rests: there is no price to rest at.
        if (order.type == OrderType::Market || order.tif == TimeInForce::Ioc) {
            report.outcome = Outcome::Cancelled;
            ++stats_.cancelled;
            return report;
        }

        // Rests at its own limit price, not at whatever it traded at.
        report.resting_ref = next_ref_++;
        book_.add(report.resting_ref, order.side, order.price, remaining, order.owner);
        report.outcome = Outcome::Rested;
        report.resting_qty = remaining;
        ++stats_.rested;
        return report;
    }

    void cancel(OrderRef ref) {
        book_.remove(ref);
        ++stats_.cancelled;
    }

    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
    [[nodiscard]] StpMode stp_mode() const noexcept { return stp_; }
    void set_stp_mode(StpMode m) noexcept { stp_ = m; }

private:
    // A buy crosses when it is willing to pay at least the offer; a sell when it
    // is willing to accept at most the bid. A market order crosses anything.
    [[nodiscard]] static bool crosses(const NewOrder& o, Price touch) noexcept {
        if (o.type == OrderType::Market) {
            return true;
        }
        return (o.side == Side::Buy) ? o.price >= touch : o.price <= touch;
    }

    Book&       book_;
    StpMode     stp_;
    OrderRef    next_ref_ = 1;
    EngineStats stats_{};
};

}  // namespace itch::engine
