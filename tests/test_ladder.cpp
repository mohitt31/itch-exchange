// The ladder against a std::map model.
//
// The model is the whole point: the ladder's correctness rests on one invariant
// -- a price is in the window if and only if it is penny aligned and its tick is
// in range, everything else is in the overflow map -- and rebasing has to move
// levels in both directions to keep that true. A map has no such invariant to
// break, so any disagreement is the ladder's.

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "harness.hpp"
#include "itch/book/price_ladder.hpp"

using namespace itch;
using namespace itch::book;

namespace {

LevelHandle h(u32 i) { return LevelHandle::make(i, 1); }

constexpr Price kPenny = 100;
constexpr Price kBase = 100'0000;  // $100.00

// Compares the ladder against an ordered map holding the same prices.
void compare(const PriceLadder& lad, const std::map<Price, u32>& model, Side side) {
    ITCH_REQUIRE_EQ(lad.size(), static_cast<u32>(model.size()));
    ITCH_REQUIRE_EQ(lad.empty(), model.empty());

    for (const auto& [price, idx] : model) {
        ITCH_TEST_CONTEXT("price=" + std::to_string(price));
        const LevelHandle got = lad.find(price);
        ITCH_REQUIRE(!got.is_null());
        ITCH_REQUIRE_EQ(got.index(), idx);
    }

    if (!model.empty()) {
        const Price expect =
            side == Side::Buy ? model.rbegin()->first : model.begin()->first;
        ITCH_REQUIRE_EQ(lad.best(), expect);
    }

    // Order of traversal must match the model's, inside outwards.
    std::vector<Price> seen;
    lad.for_each_level([&](Price p, LevelHandle) { seen.push_back(p); });
    ITCH_REQUIRE_EQ(seen.size(), model.size());
    std::vector<Price> expect;
    for (const auto& [price, idx] : model) {
        expect.push_back(price);
    }
    if (side == Side::Buy) {
        std::reverse(expect.begin(), expect.end());
    }
    for (std::size_t i = 0; i < seen.size(); ++i) {
        ITCH_REQUIRE_EQ(seen[i], expect[i]);
    }

    lad.validate();
}

}  // namespace

ITCH_TEST(ladder_starts_empty) {
    PriceLadder lad{Side::Buy};
    ITCH_CHECK(lad.empty());
    ITCH_CHECK_EQ(lad.size(), u32{0});
    ITCH_CHECK(lad.find(kBase).is_null());
    ITCH_REQUIRE_ASSERT((void)lad.best());
}

ITCH_TEST(ladder_insert_find_erase) {
    PriceLadder lad{Side::Buy};
    lad.insert(kBase, h(7));
    ITCH_CHECK_EQ(lad.find(kBase).index(), u32{7});
    ITCH_CHECK_EQ(lad.size(), u32{1});
    ITCH_CHECK_EQ(lad.best(), kBase);

    lad.erase(kBase);
    ITCH_CHECK(lad.empty());
    ITCH_CHECK(lad.find(kBase).is_null());
    lad.validate();
}

ITCH_TEST(ladder_bid_best_is_the_highest) {
    PriceLadder lad{Side::Buy};
    lad.insert(kBase, h(1));
    lad.insert(kBase + kPenny, h(2));
    lad.insert(kBase - kPenny, h(3));
    ITCH_CHECK_EQ(lad.best(), kBase + kPenny);
    lad.erase(kBase + kPenny);
    ITCH_CHECK_EQ(lad.best(), kBase);
    lad.erase(kBase);
    ITCH_CHECK_EQ(lad.best(), kBase - kPenny);
    lad.validate();
}

ITCH_TEST(ladder_ask_best_is_the_lowest) {
    PriceLadder lad{Side::Sell};
    lad.insert(kBase, h(1));
    lad.insert(kBase + kPenny, h(2));
    lad.insert(kBase - kPenny, h(3));
    ITCH_CHECK_EQ(lad.best(), kBase - kPenny);
    lad.erase(kBase - kPenny);
    ITCH_CHECK_EQ(lad.best(), kBase);
    lad.validate();
}

ITCH_TEST(ladder_best_shift_crosses_a_bitset_word) {
    // 64 ticks is one u64 of the occupancy bitset. Clearing across that boundary
    // is where a scan that only looks at one word goes wrong.
    PriceLadder lad{Side::Buy};
    for (int i = 0; i < 200; ++i) {
        lad.insert(kBase + static_cast<Price>(i) * kPenny, h(static_cast<u32>(i + 1)));
    }
    for (int i = 199; i >= 0; --i) {
        ITCH_TEST_CONTEXT("i=" + std::to_string(i));
        ITCH_REQUIRE_EQ(lad.best(), kBase + static_cast<Price>(i) * kPenny);
        lad.erase(kBase + static_cast<Price>(i) * kPenny);
    }
    ITCH_CHECK(lad.empty());
}

ITCH_TEST(ladder_sub_penny_prices_go_to_the_overflow_map) {
    // 0.0016% of real adds. They must still be found, and they must never be
    // put in the window, where the tick index cannot represent them.
    PriceLadder lad{Side::Buy};
    const Price odd = kBase + 1;  // $100.0001
    lad.insert(odd, h(9));
    ITCH_CHECK_EQ(lad.overflow_count(), std::size_t{1});
    ITCH_CHECK_EQ(lad.in_window_count(), u32{0});
    ITCH_CHECK_EQ(lad.find(odd).index(), u32{9});
    ITCH_CHECK_EQ(lad.best(), odd);
    lad.validate();

    lad.insert(kBase, h(10));
    ITCH_CHECK_EQ(lad.in_window_count(), u32{1});
    ITCH_CHECK_EQ(lad.best(), odd);  // $100.0001 beats $100.00 on the bid
    lad.validate();
}

ITCH_TEST(ladder_stub_quotes_stay_in_the_overflow_map) {
    // The measured reality: every symbol carries orders at $0.0001 and
    // $199,999.99 all session. They must never occupy a hot slot.
    PriceLadder lad{Side::Buy};
    lad.insert(kBase, h(1));
    const Price stub_low = 1;                 // $0.0001
    const Price stub_high = 1'999'999'900;    // $199,999.99
    lad.insert(stub_low, h(2));
    lad.insert(stub_high, h(3));

    ITCH_CHECK_EQ(lad.find(stub_low).index(), u32{2});
    ITCH_CHECK_EQ(lad.find(stub_high).index(), u32{3});
    // The high stub is the best bid, so the window follows it.
    ITCH_CHECK_EQ(lad.best(), stub_high);
    lad.validate();

    lad.erase(stub_high);
    ITCH_CHECK_EQ(lad.best(), kBase);
    ITCH_CHECK(!lad.find(stub_low).is_null());
    lad.validate();
}

ITCH_TEST(ladder_rebases_when_the_inside_walks_away) {
    PriceLadder lad{Side::Buy};
    lad.insert(kBase, h(1));
    const i64 first_base = lad.base_tick();

    // Walk the inside up well past the window.
    for (int i = 1; i <= 6000; ++i) {
        lad.insert(kBase + static_cast<Price>(i) * kPenny, h(static_cast<u32>(i + 1)));
    }
    ITCH_CHECK_GT(lad.rebases(), u64{0});
    ITCH_CHECK_NE(lad.base_tick(), first_base);
    ITCH_CHECK_EQ(lad.best(), kBase + 6000 * kPenny);

    // The level left far behind must still be findable, wherever it ended up.
    ITCH_CHECK_EQ(lad.find(kBase).index(), u32{1});
    ITCH_CHECK_EQ(lad.size(), u32{6001});
    ITCH_CHECK_GT(lad.levels_displaced(), u64{0});
    lad.validate();
}

ITCH_TEST(ladder_pulls_levels_back_in_when_the_window_returns) {
    // A level displaced into the overflow map has to come back into the window
    // when the window slides over it again, or the invariant that find() relies
    // on is broken and lookups start missing.
    PriceLadder lad{Side::Buy};
    lad.insert(kBase, h(1));
    for (int i = 1; i <= 6000; ++i) {
        lad.insert(kBase + static_cast<Price>(i) * kPenny, h(static_cast<u32>(i + 1)));
    }
    ITCH_REQUIRE_GT(lad.overflow_count(), std::size_t{0});

    // Now walk the inside back down by removing from the top.
    for (int i = 6000; i >= 1; --i) {
        lad.erase(kBase + static_cast<Price>(i) * kPenny);
        // Forces the rebase check on the way down.
        lad.insert(kBase + static_cast<Price>(i) * kPenny, h(static_cast<u32>(i + 1)));
        lad.erase(kBase + static_cast<Price>(i) * kPenny);
    }
    ITCH_CHECK_EQ(lad.size(), u32{1});
    ITCH_CHECK_EQ(lad.best(), kBase);
    ITCH_CHECK_EQ(lad.find(kBase).index(), u32{1});
    lad.validate();
}

ITCH_TEST(ladder_misuse_is_detected) {
    PriceLadder lad{Side::Buy};
    lad.insert(kBase, h(1));
    // Both of these are ITCH_ASSERT: on in every build, because each is a
    // compare against something already in a register.
    ITCH_REQUIRE_ASSERT(lad.insert(kBase + kPenny, LevelHandle{}));  // null handle
    ITCH_REQUIRE_ASSERT(lad.erase(kBase + kPenny));                  // not present
}

ITCH_TEST(ladder_duplicate_price_is_caught_at_invariant_level_1) {
    // Deliberately ITCH_INVARIANT and not ITCH_ASSERT. Detecting a duplicate
    // costs a full find() on the insert path, which is the hot path, so it is
    // paid for in the checked builds and not in release. Level 0 is where the
    // benchmarks run and where this check is meant to be absent.
    if (ITCH_INVARIANT_LEVEL < 1) {
        return;
    }
    PriceLadder lad{Side::Buy};
    lad.insert(kBase, h(1));
    ITCH_REQUIRE_ASSERT(lad.insert(kBase, h(2)));
}

ITCH_TEST(ladder_matches_a_map_under_random_operations) {
    constexpr u64 kSeed = 0x5EED0030;
    ITCH_TEST_CONTEXT("seed=" + std::to_string(kSeed));

    for (Side side : {Side::Buy, Side::Sell}) {
        ITCH_TEST_CONTEXT(side == Side::Buy ? "side=buy" : "side=sell");
        itch::test::Rng rng{kSeed};
        PriceLadder     lad{side};
        std::map<Price, u32> model;
        u32 next = 1;

        // A drifting centre, so the window has to rebase repeatedly and levels
        // have to move both out of and back into it.
        i64 centre = static_cast<i64>(kBase / kPenny);

        for (int step = 0; step < 30000; ++step) {
            centre += static_cast<i64>(rng.range(0, 6)) - 3;

            if (model.empty() || rng.chance(55)) {
                // Mostly near the centre, sometimes far, occasionally a stub
                // quote or a sub-penny price: the measured shape.
                Price price;
                const u64 r = rng.range(0, 999);
                if (r < 900) {
                    price = static_cast<Price>(
                        (centre + static_cast<i64>(rng.range(0, 60)) - 30) * kPenny);
                } else if (r < 985) {
                    price = static_cast<Price>(
                        (centre + static_cast<i64>(rng.range(0, 8000)) - 4000) * kPenny);
                } else if (r < 997) {
                    price = rng.chance(50) ? Price{1} : Price{1'999'999'900};
                } else {
                    price = static_cast<Price>(centre * kPenny) + 1;  // sub-penny
                }
                if (price == 0 || model.count(price) != 0) {
                    continue;
                }
                lad.insert(price, h(next));
                model.emplace(price, next);
                ++next;
            } else {
                auto it = model.begin();
                std::advance(it, static_cast<std::ptrdiff_t>(
                                     rng.range(0, model.size() - 1)));
                lad.erase(it->first);
                model.erase(it);
            }

            if (step < 300 || step % 197 == 0) {
                ITCH_TEST_CONTEXT("step=" + std::to_string(step));
                compare(lad, model, side);
            }
        }
        compare(lad, model, side);
        ITCH_CHECK_GT(lad.rebases(), u64{0});
    }
}
