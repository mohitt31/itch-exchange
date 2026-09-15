// Drives a book from an ITCH message stream, for one symbol.
//
// Symbol filtering has to go through the stock locate code, not the symbol
// text. Only the add messages carry the eight character symbol; execute,
// cancel, delete and replace identify the security purely by its locate code.
// So the builder watches the stock directory messages that NASDAQ sends at the
// start of the session, learns the locate code for the symbol it was asked for,
// and filters on that from then on.
//
// Orders belonging to other symbols never enter the index, so a later execute
// for one of them is skipped by the locate test and never looks like a
// reference to a missing order.
#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "itch/book/book_types.hpp"
#include "itch/core/assert.hpp"
#include "itch/core/types.hpp"
#include "itch/wire/views.hpp"

namespace itch::replay {

using itch::book::Side;

// Hooks for tools that need to see the book around an update. Measuring how far
// a new order sits from the inside, for instance, has to happen before the
// order is applied, because applying it can move the inside.
//
// The default does nothing and costs nothing: every hook is empty and inlines
// away, so the plain replay path is unchanged by the existence of this.
struct NullObserver {
    template <class Book>
    void before_add(const Book&, Side, Price, Qty) noexcept {}

    void note_reduction(Side, Qty) noexcept {}

    template <class Book>
    void after_apply(const Book&, Timestamp) noexcept {}
};

struct BuilderStats {
    u64 messages = 0;       // every message seen
    u64 applied = 0;        // book mutations performed
    u64 adds = 0;
    u64 executions = 0;
    u64 cancels = 0;
    u64 deletes = 0;
    u64 replaces = 0;
    u64 executed_shares = 0;
    u64 trades_ignored = 0;   // P, Q, B: reports, not book updates
    u64 crosses_ignored = 0;
    Timestamp first_ts = 0;
    Timestamp last_ts = 0;
};

template <class Book, class Observer = NullObserver>
class BookBuilder {
public:
    // An empty symbol means "decide later": the first stock directory message
    // wins. Useful for tools that just want some symbol to look at.
    explicit BookBuilder(Book& book, std::string_view symbol, Observer observer = Observer{})
        : book_(book), symbol_(symbol), observer_(std::move(observer)) {}

    [[nodiscard]] Observer& observer() noexcept { return observer_; }
    [[nodiscard]] const Observer& observer() const noexcept { return observer_; }

    // --- reference data --------------------------------------------------

    void on_stock_directory(wire::StockDirectoryView v) {
        seen(v.timestamp());
        if (resolved_) {
            return;
        }
        const std::string_view sym = wire::trim_alpha(v.stock());
        if (symbol_.empty() || sym == symbol_) {
            symbol_ = sym;
            locate_ = v.stock_locate();
            resolved_ = true;
        }
    }

    // --- book messages ---------------------------------------------------

    void on_add_order(wire::AddOrderView v) {
        seen(v.timestamp());
        if (!mine(v.stock_locate())) {
            return;
        }
        do_add(v.order_reference_number(), v.buy_sell_indicator(), v.price(), v.shares(),
               v.timestamp());
    }

    void on_add_order_with_mpid(wire::AddOrderWithMpidView v) {
        seen(v.timestamp());
        if (!mine(v.stock_locate())) {
            return;
        }
        do_add(v.order_reference_number(), v.buy_sell_indicator(), v.price(), v.shares(),
               v.timestamp());
    }

    void on_order_executed(wire::OrderExecutedView v) {
        seen(v.timestamp());
        if (!mine(v.stock_locate())) {
            return;
        }
        observer_.note_reduction(book_.side_of(v.order_reference_number()),
                                 v.executed_shares());
        book_.execute(v.order_reference_number(), v.executed_shares());
        stats_.executions++;
        stats_.executed_shares += v.executed_shares();
        stats_.applied++;
        observer_.after_apply(book_, v.timestamp());
    }

    void on_order_executed_with_price(wire::OrderExecutedWithPriceView v) {
        seen(v.timestamp());
        if (!mine(v.stock_locate())) {
            return;
        }
        // The execution price differs from the resting price, but the resting
        // order is still reduced by the executed quantity. The price on this
        // message describes the print, not the book.
        observer_.note_reduction(book_.side_of(v.order_reference_number()),
                                 v.executed_shares());
        book_.execute(v.order_reference_number(), v.executed_shares());
        stats_.executions++;
        stats_.executed_shares += v.executed_shares();
        stats_.applied++;
        observer_.after_apply(book_, v.timestamp());
    }

    void on_order_cancel(wire::OrderCancelView v) {
        seen(v.timestamp());
        if (!mine(v.stock_locate())) {
            return;
        }
        observer_.note_reduction(book_.side_of(v.order_reference_number()),
                                 v.cancelled_shares());
        book_.cancel(v.order_reference_number(), v.cancelled_shares());
        stats_.cancels++;
        stats_.applied++;
        observer_.after_apply(book_, v.timestamp());
    }

    void on_order_delete(wire::OrderDeleteView v) {
        seen(v.timestamp());
        if (!mine(v.stock_locate())) {
            return;
        }
        observer_.note_reduction(book_.side_of(v.order_reference_number()),
                                 book_.qty_of(v.order_reference_number()));
        book_.remove(v.order_reference_number());
        stats_.deletes++;
        stats_.applied++;
        observer_.after_apply(book_, v.timestamp());
    }

    void on_order_replace(wire::OrderReplaceView v) {
        seen(v.timestamp());
        if (!mine(v.stock_locate())) {
            return;
        }
        // A replace is a delete plus an add, so the observer sees both halves
        // the same way it would see them standalone.
        const Side rside = book_.side_of(v.original_order_reference_number());
        observer_.note_reduction(rside, book_.qty_of(v.original_order_reference_number()));
        observer_.before_add(book_, rside, v.price(), v.shares());
        book_.replace(v.original_order_reference_number(), v.new_order_reference_number(),
                      v.price(), v.shares());
        stats_.replaces++;
        stats_.applied++;
        observer_.after_apply(book_, v.timestamp());
    }

    // --- reports that must not touch the book ----------------------------

    void on_trade_non_cross(wire::TradeNonCrossView v) {
        seen(v.timestamp());
        if (mine(v.stock_locate())) {
            stats_.trades_ignored++;
        }
    }

    void on_cross_trade(wire::CrossTradeView v) {
        seen(v.timestamp());
        if (mine(v.stock_locate())) {
            stats_.crosses_ignored++;
        }
    }

    void on_broken_trade(wire::BrokenTradeView v) {
        seen(v.timestamp());
        if (mine(v.stock_locate())) {
            stats_.trades_ignored++;
        }
    }

    // --- accessors -------------------------------------------------------

    [[nodiscard]] const BuilderStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::string_view symbol() const noexcept { return symbol_; }
    [[nodiscard]] u16 locate() const noexcept { return locate_; }
    [[nodiscard]] bool resolved() const noexcept { return resolved_; }

private:
    void seen(Timestamp ts) noexcept {
        if (stats_.messages == 0) {
            stats_.first_ts = ts;
        }
        stats_.last_ts = ts;
        stats_.messages++;
    }

    [[nodiscard]] bool mine(u16 locate) const noexcept {
        return resolved_ && locate == locate_;
    }

    void do_add(OrderRef ref, char indicator, Price price, Qty shares, Timestamp ts) {
        ITCH_ASSERT_MSG(indicator == 'B' || indicator == 'S',
                        "buy/sell indicator is neither B nor S");
        const Side side = indicator == 'B' ? Side::Buy : Side::Sell;
        observer_.before_add(book_, side, price, shares);
        book_.add(ref, side, price, shares);
        stats_.adds++;
        stats_.applied++;
        observer_.after_apply(book_, ts);
    }

    Book&        book_;
    std::string  symbol_;
    Observer     observer_;
    u16          locate_ = 0;
    bool         resolved_ = false;
    BuilderStats stats_{};
};

}  // namespace itch::replay
