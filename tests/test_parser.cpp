#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "harness.hpp"
#include "itch/wire/framing.hpp"
#include "itch/wire/views.hpp"
#include "wire_builder.hpp"

using namespace itch;
using namespace itch::wire;
using itch::test::WireBuilder;

namespace {

// Records which handler method ran, and for which message type.
struct RecordingHandler {
    std::vector<std::string> calls;
    std::vector<char>        types;

    void note(const char* what, char t) {
        calls.emplace_back(what);
        types.push_back(t);
    }

    void on_system_event(SystemEventView v) { note("system_event", v.kType); }
    void on_stock_directory(StockDirectoryView v) { note("stock_directory", v.kType); }
    void on_stock_trading_action(StockTradingActionView v) {
        note("stock_trading_action", v.kType);
    }
    void on_reg_sho_restriction(RegShoRestrictionView v) {
        note("reg_sho_restriction", v.kType);
    }
    void on_market_participant_position(MarketParticipantPositionView v) {
        note("market_participant_position", v.kType);
    }
    void on_mwcb_decline_level(MwcbDeclineLevelView v) { note("mwcb_decline_level", v.kType); }
    void on_mwcb_status(MwcbStatusView v) { note("mwcb_status", v.kType); }
    void on_ipo_quoting_period_update(IpoQuotingPeriodUpdateView v) {
        note("ipo_quoting_period_update", v.kType);
    }
    void on_luld_auction_collar(LuldAuctionCollarView v) { note("luld_auction_collar", v.kType); }
    void on_operational_halt(OperationalHaltView v) { note("operational_halt", v.kType); }
    void on_add_order(AddOrderView v) { note("add_order", v.kType); }
    void on_add_order_with_mpid(AddOrderWithMpidView v) { note("add_order_with_mpid", v.kType); }
    void on_order_executed(OrderExecutedView v) { note("order_executed", v.kType); }
    void on_order_executed_with_price(OrderExecutedWithPriceView v) {
        note("order_executed_with_price", v.kType);
    }
    void on_order_cancel(OrderCancelView v) { note("order_cancel", v.kType); }
    void on_order_delete(OrderDeleteView v) { note("order_delete", v.kType); }
    void on_order_replace(OrderReplaceView v) { note("order_replace", v.kType); }
    void on_trade_non_cross(TradeNonCrossView v) { note("trade_non_cross", v.kType); }
    void on_cross_trade(CrossTradeView v) { note("cross_trade", v.kType); }
    void on_broken_trade(BrokenTradeView v) { note("broken_trade", v.kType); }
    void on_noii(NoiiView v) { note("noii", v.kType); }
    void on_retail_price_improvement(RetailPriceImprovementView v) {
        note("retail_price_improvement", v.kType);
    }
    void on_direct_listing_with_capital_raise(DirectListingWithCapitalRaiseView v) {
        note("direct_listing_with_capital_raise", v.kType);
    }
};

// Implements exactly one method. Everything else must be skipped at compile
// time rather than requiring an empty override.
struct SparseHandler {
    int adds = 0;
    void on_add_order(AddOrderView) { ++adds; }
};

std::vector<char> all_types() {
    std::vector<char> t;
    for (int c = 0; c < 256; ++c) {
        if (is_known_type(static_cast<char>(c))) {
            t.push_back(static_cast<char>(c));
        }
    }
    return t;
}

}  // namespace

ITCH_TEST(parser_add_order_round_trip) {
    WireBuilder b;
    const std::size_t m = b.begin('A');
    b.put<u16>(m, offsetof(AddOrder, stock_locate), 1234);
    b.put<u16>(m, offsetof(AddOrder, tracking_number), 7);
    b.put_char(m, offsetof(AddOrder, buy_sell_indicator), 'B');
    b.put<u32>(m, offsetof(AddOrder, shares), 250);
    b.put_alpha(m, offsetof(AddOrder, stock), 8, "AAPL");
    b.put<u32>(m, offsetof(AddOrder, price), 1234500);  // $123.45
    // Timestamp is 48 bit: use a value that needs all six bytes.
    b.put_be48(m, offsetof(AddOrder, timestamp), 0x1234'5678'9ABCULL);

    FrameCursor cur{b.span()};
    std::span<const std::byte> f;
    ITCH_REQUIRE(cur.next(f) == FrameStatus::Ok);

    const AddOrderView v{f.data()};
    ITCH_CHECK_EQ(v.stock_locate(), u16{1234});
    ITCH_CHECK_EQ(v.tracking_number(), u16{7});
    ITCH_CHECK_EQ(v.timestamp(), u64{0x1234'5678'9ABCULL});
    ITCH_CHECK_EQ(v.buy_sell_indicator(), 'B');
    ITCH_CHECK_EQ(v.shares(), u32{250});
    ITCH_CHECK_EQ(v.stock(), std::string_view("AAPL    "));
    ITCH_CHECK_EQ(trim_alpha(v.stock()), std::string_view("AAPL"));
    ITCH_CHECK_EQ(v.price(), u32{1234500});
}

ITCH_TEST(parser_order_replace_carries_two_references) {
    WireBuilder b;
    const std::size_t m = b.begin('U');
    b.put<u64>(m, offsetof(OrderReplace, original_order_reference_number), 111);
    b.put<u64>(m, offsetof(OrderReplace, new_order_reference_number), 222);
    b.put<u32>(m, offsetof(OrderReplace, shares), 900);
    b.put<u32>(m, offsetof(OrderReplace, price), 500000);

    const OrderReplaceView v{b.span().data() + itch::wire::kLengthPrefixSize};
    ITCH_CHECK_EQ(v.original_order_reference_number(), u64{111});
    ITCH_CHECK_EQ(v.new_order_reference_number(), u64{222});
    ITCH_CHECK_EQ(v.shares(), u32{900});
    ITCH_CHECK_EQ(v.price(), u32{500000});
}

ITCH_TEST(parser_executed_with_price_round_trip) {
    WireBuilder b;
    const std::size_t m = b.begin('C');
    b.put<u64>(m, offsetof(OrderExecutedWithPrice, order_reference_number), 42);
    b.put<u32>(m, offsetof(OrderExecutedWithPrice, executed_shares), 17);
    b.put<u64>(m, offsetof(OrderExecutedWithPrice, match_number), 0xDEADBEEFCAFEULL);
    b.put_char(m, offsetof(OrderExecutedWithPrice, printable), 'Y');
    b.put<u32>(m, offsetof(OrderExecutedWithPrice, execution_price), 99990000);

    const OrderExecutedWithPriceView v{b.span().data() + itch::wire::kLengthPrefixSize};
    ITCH_CHECK_EQ(v.order_reference_number(), u64{42});
    ITCH_CHECK_EQ(v.executed_shares(), u32{17});
    ITCH_CHECK_EQ(v.match_number(), u64{0xDEADBEEFCAFEULL});
    ITCH_CHECK_EQ(v.printable(), 'Y');
    ITCH_CHECK_EQ(v.execution_price(), u32{99990000});
}

ITCH_TEST(parser_extreme_field_values) {
    WireBuilder b;
    const std::size_t m = b.begin('A');
    b.put<u64>(m, offsetof(AddOrder, order_reference_number), u64{0xFFFF'FFFF'FFFF'FFFF});
    b.put<u32>(m, offsetof(AddOrder, shares), u32{0xFFFF'FFFF});
    b.put<u32>(m, offsetof(AddOrder, price), u32{0xFFFF'FFFF});
    b.put<u16>(m, offsetof(AddOrder, stock_locate), u16{0xFFFF});

    const AddOrderView v{b.span().data() + itch::wire::kLengthPrefixSize};
    ITCH_CHECK_EQ(v.order_reference_number(), u64{0xFFFF'FFFF'FFFF'FFFF});
    ITCH_CHECK_EQ(v.shares(), u32{0xFFFF'FFFF});
    ITCH_CHECK_EQ(v.price(), u32{0xFFFF'FFFF});
    ITCH_CHECK_EQ(v.stock_locate(), u16{0xFFFF});
}

ITCH_TEST(parser_timestamp_spans_all_48_bits) {
    // A trading day is about 8.6e13 nanoseconds, which needs 47 bits, so the
    // top bit of the 48 is genuinely reachable.
    for (u64 ts : {u64{0}, u64{1}, u64{0xFFFFFFFFFFFF}, u64{86'400'000'000'000ULL - 1}}) {
        ITCH_TEST_CONTEXT("ts=" + std::to_string(ts));
        WireBuilder b;
        const std::size_t m = b.begin('D');
        b.put_be48(m, offsetof(OrderDelete, timestamp), ts);
        const OrderDeleteView v{b.span().data() + itch::wire::kLengthPrefixSize};
        ITCH_CHECK_EQ(v.timestamp(), ts & 0xFFFFFFFFFFFFULL);
    }
}

ITCH_TEST(parser_alpha_trimming) {
    ITCH_CHECK_EQ(trim_alpha("AAPL    "), std::string_view("AAPL"));
    ITCH_CHECK_EQ(trim_alpha("        "), std::string_view(""));
    ITCH_CHECK_EQ(trim_alpha("BRK A   "), std::string_view("BRK A"));
    ITCH_CHECK_EQ(trim_alpha(""), std::string_view(""));
    // Leading spaces are not padding and must survive.
    ITCH_CHECK_EQ(trim_alpha(" X      "), std::string_view(" X"));
}

ITCH_TEST(parser_dispatch_reaches_every_type_exactly_once) {
    WireBuilder b;
    const std::vector<char> types = all_types();
    for (char t : types) {
        b.begin(t);
    }

    RecordingHandler h;
    FrameCursor cur{b.span()};
    std::span<const std::byte> f;
    while (cur.next(f) == FrameStatus::Ok) {
        ITCH_REQUIRE(dispatch(f.data(), h));
    }

    ITCH_REQUIRE_EQ(h.types.size(), types.size());
    ITCH_REQUIRE_EQ(h.types.size(), kMessageTypeCount);
    for (std::size_t i = 0; i < types.size(); ++i) {
        ITCH_TEST_CONTEXT(std::string("type='") + types[i] + "' -> " + h.calls[i]);
        // The view the handler received must be the one for that type.
        ITCH_CHECK_EQ(h.types[i], types[i]);
    }
}

ITCH_TEST(parser_dispatch_rejects_an_unknown_type) {
    const std::byte bad[1] = {std::byte{'G'}};
    RecordingHandler h;
    ITCH_CHECK(!dispatch(bad, h));
    ITCH_CHECK_EQ(h.calls.size(), std::size_t{0});
}

ITCH_TEST(parser_handler_may_implement_only_what_it_needs) {
    WireBuilder b;
    for (char t : all_types()) {
        b.begin(t);
    }
    b.begin('A');
    b.begin('A');

    SparseHandler h;
    FrameCursor cur{b.span()};
    std::span<const std::byte> f;
    while (cur.next(f) == FrameStatus::Ok) {
        ITCH_REQUIRE(dispatch(f.data(), h));
    }
    // One 'A' from the full sweep plus the two appended.
    ITCH_CHECK_EQ(h.adds, 3);
}

ITCH_TEST(parser_view_sizes_agree_with_the_length_table) {
    ITCH_CHECK_EQ(AddOrderView::kSize, message_length('A'));
    ITCH_CHECK_EQ(AddOrderWithMpidView::kSize, message_length('F'));
    ITCH_CHECK_EQ(OrderDeleteView::kSize, message_length('D'));
    ITCH_CHECK_EQ(NoiiView::kSize, message_length('I'));
    ITCH_CHECK_EQ(DirectListingWithCapitalRaiseView::kSize, message_length('O'));
}

ITCH_TEST(parser_type_names_are_distinct_and_present) {
    std::vector<std::string> names;
    for (char t : all_types()) {
        const std::string n = type_name(t);
        ITCH_REQUIRE_NE(n, std::string("unknown"));
        names.push_back(n);
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        for (std::size_t j = i + 1; j < names.size(); ++j) {
            ITCH_REQUIRE_NE(names[i], names[j]);
        }
    }
    ITCH_CHECK_EQ(std::string(type_name('G')), std::string("unknown"));
}
