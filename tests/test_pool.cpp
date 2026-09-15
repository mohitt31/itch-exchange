#include <string>
#include <vector>

#include "harness.hpp"
#include "itch/book/pool.hpp"
#include "itch/book/records.hpp"

using namespace itch;
using namespace itch::book;

namespace {

using OrderPool = Pool<Order, OrderTag>;
using LevelPool = Pool<Level, LevelTag>;

}  // namespace

ITCH_TEST(handle_null_is_distinct_from_every_real_handle) {
    ITCH_CHECK(OrderHandle{}.is_null());
    ITCH_CHECK(!OrderHandle{});
    for (u32 gen = 0; gen < 256; ++gen) {
        const auto h = OrderHandle::make(OrderHandle::kMaxCapacity - 1, static_cast<u8>(gen));
        ITCH_REQUIRE(!h.is_null());
    }
}

ITCH_TEST(handle_round_trips_index_and_generation) {
    for (u32 idx : {u32{0}, u32{1}, u32{255}, u32{256}, u32{65535},
                    OrderHandle::kMaxCapacity - 1}) {
        for (u32 gen : {u32{0}, u32{1}, u32{127}, u32{255}}) {
            ITCH_TEST_CONTEXT("idx=" + std::to_string(idx) + " gen=" + std::to_string(gen));
            const auto h = OrderHandle::make(idx, static_cast<u8>(gen));
            ITCH_REQUIRE_EQ(h.index(), idx);
            ITCH_REQUIRE_EQ(h.generation(), static_cast<u8>(gen));
        }
    }
}

ITCH_TEST(pool_allocates_distinct_live_slots) {
    OrderPool pool{64};
    ITCH_CHECK_EQ(pool.live(), u32{0});

    std::vector<OrderHandle> hs;
    for (int i = 0; i < 64; ++i) {
        const auto h = pool.allocate();
        ITCH_REQUIRE(pool.is_live(h));
        pool[h].ref = static_cast<OrderRef>(i);
        hs.push_back(h);
    }
    ITCH_CHECK_EQ(pool.live(), u32{64});
    ITCH_CHECK_EQ(pool.high_water(), u32{64});

    // Every slot must be distinct, which is checkable through the data.
    for (int i = 0; i < 64; ++i) {
        ITCH_REQUIRE_EQ(pool[hs[static_cast<std::size_t>(i)]].ref, static_cast<OrderRef>(i));
    }
    pool.validate();
}

ITCH_TEST(pool_exhaustion_is_detected_not_wrapped) {
    OrderPool pool{4};
    for (int i = 0; i < 4; ++i) {
        (void)pool.allocate();
    }
    ITCH_REQUIRE_ASSERT((void)pool.allocate());
}

ITCH_TEST(pool_rejects_an_impossible_capacity) {
    ITCH_REQUIRE_ASSERT((OrderPool{0}));
    ITCH_REQUIRE_ASSERT((OrderPool{OrderHandle::kMaxCapacity + 1}));
}

ITCH_TEST(pool_reuses_freed_slots) {
    OrderPool pool{8, /*quarantine=*/0};
    std::vector<OrderHandle> hs;
    for (int i = 0; i < 8; ++i) {
        hs.push_back(pool.allocate());
    }
    for (const auto h : hs) {
        pool.free(h);
    }
    ITCH_CHECK_EQ(pool.live(), u32{0});
    for (int i = 0; i < 8; ++i) {
        const auto h = pool.allocate();
        ITCH_REQUIRE(pool.is_live(h));
    }
    ITCH_CHECK_EQ(pool.live(), u32{8});
}

ITCH_TEST(pool_detects_use_after_free) {
    OrderPool pool{16};
    const auto h = pool.allocate();
    pool[h].ref = 42;
    ITCH_CHECK(pool.is_live(h));

    pool.free(h);
    ITCH_CHECK(!pool.is_live(h));
    ITCH_REQUIRE_ASSERT(pool[h].ref = 1);
}

ITCH_TEST(pool_detects_double_free) {
    OrderPool pool{16};
    const auto h = pool.allocate();
    pool.free(h);
    ITCH_REQUIRE_ASSERT(pool.free(h));
}

ITCH_TEST(pool_detects_a_null_handle) {
    OrderPool pool{16};
    ITCH_CHECK(!pool.is_live(OrderHandle{}));
    ITCH_REQUIRE_ASSERT(pool[OrderHandle{}].ref = 1);
}

ITCH_TEST(pool_detects_an_out_of_range_handle) {
    OrderPool pool{16};
    const auto bad = OrderHandle::make(1000, 1);
    ITCH_CHECK(!pool.is_live(bad));
    ITCH_REQUIRE_ASSERT(pool[bad].ref = 1);
}

ITCH_TEST(pool_detects_a_handle_to_a_reused_slot) {
    // The slot comes back, but with a new generation, so the old handle must be
    // rejected rather than silently reading the new occupant.
    OrderPool pool{1, /*quarantine=*/0};
    const auto first = pool.allocate();
    pool[first].ref = 111;
    pool.free(first);

    const auto second = pool.allocate();
    pool[second].ref = 222;

    ITCH_CHECK_EQ(first.index(), second.index());
    ITCH_CHECK_NE(first.generation(), second.generation());
    ITCH_CHECK(!pool.is_live(first));
    ITCH_CHECK(pool.is_live(second));
    ITCH_REQUIRE_ASSERT(pool[first].ref = 1);
    ITCH_CHECK_EQ(pool[second].ref, OrderRef{222});
}

ITCH_TEST(pool_generation_is_odd_while_live_and_even_while_free) {
    OrderPool pool{4, /*quarantine=*/0};
    const auto h = pool.allocate();
    ITCH_CHECK_EQ(h.generation() & 1u, u32{1});
    pool.free(h);
    // A stale handle's generation no longer matches what the slot holds.
    ITCH_CHECK(!pool.is_live(h));
}

ITCH_TEST(pool_generation_wraps_without_quarantine) {
    // Recorded rather than hidden: with the quarantine off, a slot recycled 128
    // times comes back with the generation the stale handle remembers, and the
    // handle is accepted. That is the documented limit of an 8-bit generation
    // whose low bit is the live flag. The sanitizer build's quarantine is what
    // makes this unreachable in practice.
    OrderPool pool{1, /*quarantine=*/0};
    const auto stale = pool.allocate();
    pool[stale].ref = 111;
    pool.free(stale);

    for (int i = 0; i < 127; ++i) {
        const auto churn = pool.allocate();
        pool.free(churn);
    }
    ITCH_CHECK(!pool.is_live(stale));  // 127 cycles: still caught

    const auto reborn = pool.allocate();
    ITCH_CHECK_EQ(reborn.generation(), stale.generation());
    ITCH_CHECK(pool.is_live(stale));   // 128 cycles: the wrap, as documented
}

ITCH_TEST(pool_free_list_stays_well_formed) {
    constexpr u64 kSeed = 0x5EED0020;
    ITCH_TEST_CONTEXT("seed=" + std::to_string(kSeed));
    itch::test::Rng rng{kSeed};

    OrderPool pool{512, /*quarantine=*/0};
    std::vector<OrderHandle> live;
    std::vector<OrderHandle> dead;

    for (int step = 0; step < 20000; ++step) {
        if (live.empty() || (rng.chance(55) && pool.live() < 500)) {
            const auto h = pool.allocate();
            pool[h].ref = static_cast<OrderRef>(step);
            live.push_back(h);
        } else {
            const auto i = static_cast<std::size_t>(rng.range(0, live.size() - 1));
            const auto h = live[i];
            pool.free(h);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
            dead.push_back(h);
        }

        if (step % 251 == 0) {
            ITCH_TEST_CONTEXT("step=" + std::to_string(step));
            pool.validate();
            ITCH_REQUIRE_EQ(pool.live(), static_cast<u32>(live.size()));
            for (const auto h : live) {
                ITCH_REQUIRE(pool.is_live(h));
            }
        }
    }
    pool.validate();
    ITCH_CHECK_GT(pool.high_water(), u32{0});
}

ITCH_TEST(pool_quarantine_makes_stale_handles_certain_to_be_caught) {
    // Without a quarantine, a slot recycled 128 times comes back with the same
    // generation and a stale handle is accepted. The sanitizer build holds
    // freed slots back so that cannot happen within a test's lifetime.
    if (kPoolQuarantine == 0) {
        return;  // release build: nothing to assert
    }
    OrderPool pool{static_cast<u32>(kPoolQuarantine) + 16};
    const auto stale = pool.allocate();
    pool.free(stale);

    // Churn far past the 128-cycle generation wrap.
    for (int i = 0; i < 4000; ++i) {
        const auto churn = pool.allocate();
        pool.free(churn);
    }
    ITCH_CHECK(!pool.is_live(stale));
    ITCH_REQUIRE_ASSERT(pool[stale].ref = 1);
}

ITCH_TEST(pool_level_records_work_the_same_way) {
    LevelPool pool{32, /*quarantine=*/0};
    const auto a = pool.allocate();
    const auto b = pool.allocate();
    pool[a].price = 100'0000;
    pool[b].price = 100'0100;
    ITCH_CHECK_EQ(pool[a].price, Price{100'0000});
    ITCH_CHECK_EQ(pool[b].price, Price{100'0100});
    ITCH_CHECK_NE(a.index(), b.index());
    pool.free(a);
    ITCH_REQUIRE_ASSERT(pool[a].price = 1);
    ITCH_CHECK_EQ(pool[b].price, Price{100'0100});
    pool.validate();
}

ITCH_TEST(records_are_the_documented_size) {
    ITCH_CHECK_EQ(sizeof(Order), std::size_t{32});
    ITCH_CHECK_EQ(sizeof(Level), std::size_t{32});
    // Four orders per M4 cache line.
    ITCH_CHECK_EQ(std::size_t{128} / sizeof(Order), std::size_t{4});
}
