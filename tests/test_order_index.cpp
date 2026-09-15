// The order index against std::unordered_map.
//
// The part with somewhere to hide is backward-shift deletion. Linear probing
// promises that every live entry is reachable from its ideal slot without
// crossing an empty one; deleting an entry can break that for the entries
// behind it, and the repair has to move them back. Get it wrong and lookups
// start missing entries that are still in the table -- silently, and only for
// some key orders.

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "harness.hpp"
#include "itch/book/order_index.hpp"

using namespace itch;
using namespace itch::book;

namespace {

OrderHandle h(u32 i) { return OrderHandle::make(i, 1); }

template <IndexHash H>
void random_churn(u64 seed) {
    ITCH_TEST_CONTEXT("seed=" + std::to_string(seed));
    itch::test::Rng rng{seed};

    OrderIndex<H> idx{4096};
    std::unordered_map<OrderRef, u32> model;
    std::vector<OrderRef> live;
    OrderRef next = 1;

    for (int step = 0; step < 60000; ++step) {
        // Mostly sequential references, as NASDAQ issues them, with occasional
        // large jumps so the table is not only ever fed one arithmetic series.
        if (live.empty() || (rng.chance(55) && idx.size() < 4000)) {
            OrderRef ref = next++;
            if (rng.chance(3)) {
                next += rng.range(1, 1'000'000);
                ref = next++;
            }
            if (model.count(ref) != 0) {
                continue;
            }
            const u32 v = static_cast<u32>(step % 1000 + 1);
            idx.insert(ref, h(v));
            model.emplace(ref, v);
            live.push_back(ref);
        } else {
            const auto i = static_cast<std::size_t>(rng.range(0, live.size() - 1));
            const OrderRef ref = live[i];
            idx.erase(ref);
            model.erase(ref);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
        }

        if (step < 500 || step % 311 == 0) {
            ITCH_TEST_CONTEXT("step=" + std::to_string(step));
            ITCH_REQUIRE_EQ(idx.size(), static_cast<u32>(model.size()));
            for (const OrderRef r : live) {
                const OrderHandle got = idx.find(r);
                ITCH_REQUIRE(!got.is_null());
                ITCH_REQUIRE_EQ(got.index(), model.at(r));
            }
            idx.validate();
        }
    }
    idx.validate();
}

}  // namespace

ITCH_TEST(index_insert_find_erase) {
    OrderIndex<IndexHash::Mixed> idx{64};
    ITCH_CHECK_EQ(idx.size(), u32{0});
    ITCH_CHECK(idx.find(1).is_null());

    idx.insert(42, h(7));
    ITCH_CHECK_EQ(idx.size(), u32{1});
    ITCH_CHECK_EQ(idx.find(42).index(), u32{7});
    ITCH_CHECK(idx.find(43).is_null());

    idx.erase(42);
    ITCH_CHECK_EQ(idx.size(), u32{0});
    ITCH_CHECK(idx.find(42).is_null());
    idx.validate();
}

ITCH_TEST(index_capacity_is_a_power_of_two_and_at_least_double) {
    for (u32 hint : {u32{1}, u32{10}, u32{100}, u32{1000}, u32{65536}}) {
        ITCH_TEST_CONTEXT("hint=" + std::to_string(hint));
        OrderIndex<IndexHash::Mixed> idx{hint};
        ITCH_REQUIRE_GE(idx.capacity(), std::size_t{2} * hint);
        ITCH_REQUIRE_EQ(idx.capacity() & (idx.capacity() - 1), std::size_t{0});
    }
}

ITCH_TEST(index_refuses_to_pass_half_full) {
    // It never resizes during replay: growing mid-run would move every entry
    // and put a multi-millisecond spike into the latency tail.
    OrderIndex<IndexHash::Mixed> idx{8};  // capacity 16
    for (u64 i = 1; i <= 8; ++i) {
        idx.insert(i, h(static_cast<u32>(i)));
    }
    ITCH_REQUIRE_ASSERT(idx.insert(9, h(9)));
}

ITCH_TEST(index_erasing_a_missing_reference_is_detected) {
    OrderIndex<IndexHash::Mixed> idx{64};
    idx.insert(1, h(1));
    ITCH_REQUIRE_ASSERT(idx.erase(2));
}

ITCH_TEST(index_rejects_a_null_handle) {
    OrderIndex<IndexHash::Mixed> idx{64};
    ITCH_REQUIRE_ASSERT(idx.insert(1, OrderHandle{}));
}

ITCH_TEST(index_deletion_repairs_probe_chains) {
    // Force a collision chain by hand, then delete from the front of it. Under
    // identity hashing, keys 0, N, 2N all land on slot 0 of an N-slot table.
    OrderIndex<IndexHash::Identity> idx{8};  // capacity 16
    const u64 n = 16;
    idx.insert(0, h(1));
    idx.insert(n, h(2));
    idx.insert(2 * n, h(3));
    idx.insert(3 * n, h(4));
    idx.validate();

    // Deleting the head of the chain must move the rest back, not leave a hole
    // that later lookups stop at.
    idx.erase(0);
    idx.validate();
    ITCH_CHECK(idx.find(0).is_null());
    ITCH_CHECK_EQ(idx.find(n).index(), u32{2});
    ITCH_CHECK_EQ(idx.find(2 * n).index(), u32{3});
    ITCH_CHECK_EQ(idx.find(3 * n).index(), u32{4});

    // Now from the middle.
    idx.erase(2 * n);
    idx.validate();
    ITCH_CHECK_EQ(idx.find(n).index(), u32{2});
    ITCH_CHECK(idx.find(2 * n).is_null());
    ITCH_CHECK_EQ(idx.find(3 * n).index(), u32{4});
}

ITCH_TEST(index_wraps_around_the_end_of_the_table) {
    // A chain that starts near the top and wraps past slot 0 is the case the
    // cyclic interval test in the deletion exists for.
    OrderIndex<IndexHash::Identity> idx{8};  // capacity 16
    idx.insert(15, h(1));   // slot 15
    idx.insert(31, h(2));   // slot 15 -> probes to 0
    idx.insert(47, h(3));   // slot 15 -> probes to 1
    idx.validate();

    idx.erase(15);
    idx.validate();
    ITCH_CHECK(idx.find(15).is_null());
    ITCH_CHECK_EQ(idx.find(31).index(), u32{2});
    ITCH_CHECK_EQ(idx.find(47).index(), u32{3});
}

ITCH_TEST(index_mixed_hash_matches_a_map_under_churn) {
    random_churn<IndexHash::Mixed>(0x5EED0050);
}

ITCH_TEST(index_identity_hash_matches_a_map_under_churn) {
    random_churn<IndexHash::Identity>(0x5EED0050);
}

ITCH_TEST(index_both_hashes_agree_on_contents) {
    // Different probe orders, identical answers.
    itch::test::Rng rng{0x5EED0051};
    OrderIndex<IndexHash::Mixed>    a{2048};
    OrderIndex<IndexHash::Identity> b{2048};
    std::vector<OrderRef> refs;
    for (u64 i = 1; i <= 1500; ++i) {
        const OrderRef r = i * 3 + rng.range(0, 2);
        if (std::find(refs.begin(), refs.end(), r) != refs.end()) {
            continue;
        }
        refs.push_back(r);
        a.insert(r, h(static_cast<u32>(i)));
        b.insert(r, h(static_cast<u32>(i)));
    }
    for (const OrderRef r : refs) {
        ITCH_REQUIRE_EQ(a.find(r).bits(), b.find(r).bits());
    }
    ITCH_CHECK_EQ(a.size(), b.size());
    a.validate();
    b.validate();
}
