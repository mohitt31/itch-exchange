#include <array>
#include <cstddef>

#include "harness.hpp"
#include "itch/wire/messages.hpp"

using namespace itch;
using namespace itch::wire;

namespace {

// Written out by hand from the specification, deliberately independent of
// tools/extract_spec.py. If the extractor ever mis-parses a table, the
// generated header and this table disagree and this test fails. One of the two
// has to be wrong, and neither can quietly drift.
struct Expected {
    char        type;
    std::size_t size;
};

constexpr std::array<Expected, 23> kExpected = {{
    {'S', 12}, {'R', 39}, {'H', 25}, {'Y', 20}, {'L', 26}, {'V', 35},
    {'W', 12}, {'K', 28}, {'J', 35}, {'h', 21}, {'A', 36}, {'F', 40},
    {'E', 31}, {'C', 36}, {'X', 23}, {'D', 19}, {'U', 35}, {'P', 44},
    {'Q', 40}, {'B', 19}, {'I', 50}, {'N', 20}, {'O', 48},
}};

}  // namespace

ITCH_TEST(wire_lengths_match_an_independent_table) {
    for (const Expected& e : kExpected) {
        ITCH_TEST_CONTEXT(std::string("type='") + e.type + "'");
        ITCH_CHECK_EQ(message_length(e.type), e.size);
    }
}

ITCH_TEST(wire_type_count_matches) {
    ITCH_CHECK_EQ(kMessageTypeCount, kExpected.size());
}

ITCH_TEST(wire_max_length_is_the_real_max) {
    std::size_t max_seen = 0;
    for (const Expected& e : kExpected) {
        max_seen = (e.size > max_seen) ? e.size : max_seen;
    }
    // A framing buffer smaller than this lets a message straddle its end.
    ITCH_CHECK_EQ(kMaxMessageLength, max_seen);
    ITCH_CHECK_EQ(kMaxMessageLength, std::size_t{50});
}

ITCH_TEST(wire_unknown_types_are_rejected) {
    // Every byte that is not one of the 23 known types must report length 0.
    // 'G' and 'Z' in particular are not ITCH 5.0 message types.
    for (int c = 0; c < 256; ++c) {
        const char ch = static_cast<char>(c);
        bool known = false;
        for (const Expected& e : kExpected) {
            if (e.type == ch) {
                known = true;
                break;
            }
        }
        ITCH_TEST_CONTEXT("byte=" + std::to_string(c));
        ITCH_REQUIRE_EQ(is_known_type(ch), known);
        if (!known) {
            ITCH_REQUIRE_EQ(message_length(ch), std::size_t{0});
        }
    }
}

// Every message begins with the same four fields at the same offsets. The
// parser reads stock locate and timestamp before it knows which message it is
// holding, so this is a load-bearing property, not a coincidence.
#define ITCH_CHECK_COMMON_HEADER(T)                                            \
    do {                                                                       \
        ITCH_CHECK_EQ(offsetof(T, message_type), std::size_t{0});              \
        ITCH_CHECK_EQ(sizeof(T::message_type), std::size_t{1});                \
        ITCH_CHECK_EQ(offsetof(T, stock_locate), std::size_t{1});              \
        ITCH_CHECK_EQ(sizeof(T::stock_locate), std::size_t{2});                \
        ITCH_CHECK_EQ(offsetof(T, tracking_number), std::size_t{3});           \
        ITCH_CHECK_EQ(sizeof(T::tracking_number), std::size_t{2});             \
        ITCH_CHECK_EQ(offsetof(T, timestamp), std::size_t{5});                 \
        ITCH_CHECK_EQ(sizeof(T::timestamp), std::size_t{6});                   \
        ITCH_CHECK_EQ(alignof(T), std::size_t{1});                             \
    } while (0)

ITCH_TEST(wire_common_header_is_identical_everywhere) {
    ITCH_CHECK_COMMON_HEADER(SystemEvent);
    ITCH_CHECK_COMMON_HEADER(StockDirectory);
    ITCH_CHECK_COMMON_HEADER(StockTradingAction);
    ITCH_CHECK_COMMON_HEADER(RegShoRestriction);
    ITCH_CHECK_COMMON_HEADER(MarketParticipantPosition);
    ITCH_CHECK_COMMON_HEADER(MwcbDeclineLevel);
    ITCH_CHECK_COMMON_HEADER(MwcbStatus);
    ITCH_CHECK_COMMON_HEADER(IpoQuotingPeriodUpdate);
    ITCH_CHECK_COMMON_HEADER(LuldAuctionCollar);
    ITCH_CHECK_COMMON_HEADER(OperationalHalt);
    ITCH_CHECK_COMMON_HEADER(AddOrder);
    ITCH_CHECK_COMMON_HEADER(AddOrderWithMpid);
    ITCH_CHECK_COMMON_HEADER(OrderExecuted);
    ITCH_CHECK_COMMON_HEADER(OrderExecutedWithPrice);
    ITCH_CHECK_COMMON_HEADER(OrderCancel);
    ITCH_CHECK_COMMON_HEADER(OrderDelete);
    ITCH_CHECK_COMMON_HEADER(OrderReplace);
    ITCH_CHECK_COMMON_HEADER(TradeNonCross);
    ITCH_CHECK_COMMON_HEADER(CrossTrade);
    ITCH_CHECK_COMMON_HEADER(BrokenTrade);
    ITCH_CHECK_COMMON_HEADER(Noii);
    ITCH_CHECK_COMMON_HEADER(RetailPriceImprovement);
    ITCH_CHECK_COMMON_HEADER(DirectListingWithCapitalRaise);
}

// The five order-book messages carry the order reference at the same offset.
// The book path depends on this.
ITCH_TEST(wire_order_reference_offset_is_shared) {
    ITCH_CHECK_EQ(offsetof(AddOrder, order_reference_number), std::size_t{11});
    ITCH_CHECK_EQ(offsetof(AddOrderWithMpid, order_reference_number), std::size_t{11});
    ITCH_CHECK_EQ(offsetof(OrderExecuted, order_reference_number), std::size_t{11});
    ITCH_CHECK_EQ(offsetof(OrderExecutedWithPrice, order_reference_number), std::size_t{11});
    ITCH_CHECK_EQ(offsetof(OrderCancel, order_reference_number), std::size_t{11});
    ITCH_CHECK_EQ(offsetof(OrderDelete, order_reference_number), std::size_t{11});
    ITCH_CHECK_EQ(offsetof(TradeNonCross, order_reference_number), std::size_t{11});
    // Replace is the exception: it carries two references.
    ITCH_CHECK_EQ(offsetof(OrderReplace, original_order_reference_number), std::size_t{11});
    ITCH_CHECK_EQ(offsetof(OrderReplace, new_order_reference_number), std::size_t{19});
}

// AddOrder is a strict prefix of AddOrderWithMpid. The parser relies on this to
// decode 'A' and 'F' through one path.
ITCH_TEST(wire_add_order_is_a_prefix_of_add_order_with_mpid) {
    ITCH_CHECK_EQ(offsetof(AddOrder, buy_sell_indicator),
                  offsetof(AddOrderWithMpid, buy_sell_indicator));
    ITCH_CHECK_EQ(offsetof(AddOrder, shares), offsetof(AddOrderWithMpid, shares));
    ITCH_CHECK_EQ(offsetof(AddOrder, stock), offsetof(AddOrderWithMpid, stock));
    ITCH_CHECK_EQ(offsetof(AddOrder, price), offsetof(AddOrderWithMpid, price));
    ITCH_CHECK_EQ(sizeof(AddOrderWithMpid), sizeof(AddOrder) + 4);
}

// Likewise OrderExecuted is a prefix of OrderExecutedWithPrice.
ITCH_TEST(wire_order_executed_is_a_prefix_of_with_price) {
    ITCH_CHECK_EQ(offsetof(OrderExecuted, executed_shares),
                  offsetof(OrderExecutedWithPrice, executed_shares));
    ITCH_CHECK_EQ(offsetof(OrderExecuted, match_number),
                  offsetof(OrderExecutedWithPrice, match_number));
    ITCH_CHECK_EQ(sizeof(OrderExecutedWithPrice), sizeof(OrderExecuted) + 5);
}

ITCH_TEST(wire_message_length_is_constexpr) {
    // Guards against message_length losing constexpr, which the framing layer
    // needs for its jump table.
    static_assert(message_length('A') == 36);
    static_assert(message_length('D') == 19);
    static_assert(message_length('\0') == 0);
    static_assert(is_known_type('I'));
    static_assert(!is_known_type('G'));
    ITCH_CHECK(true);
}
