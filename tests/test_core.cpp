#include <array>
#include <cstddef>

#include "harness.hpp"
#include "itch/core/assert.hpp"
#include "itch/core/endian.hpp"
#include "itch/core/hash.hpp"
#include "itch/core/types.hpp"

using namespace itch;

namespace {

// 0x12 0x34 ... as a byte buffer, plus three bytes of lead-in so that every
// load below is deliberately unaligned.
constexpr std::array<std::byte, 16> kBuf = {
    std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC},  // lead-in
    std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78},
    std::byte{0x9A}, std::byte{0xBC}, std::byte{0xDE}, std::byte{0xF0},
    std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
    std::byte{0x05},
};

}  // namespace

ITCH_TEST(endian_load_known_values) {
    const std::byte* p = kBuf.data() + 3;
    ITCH_CHECK_EQ(load_be<u8>(p), u8{0x12});
    ITCH_CHECK_EQ(load_be<u16>(p), u16{0x1234});
    ITCH_CHECK_EQ(load_be<u32>(p), u32{0x12345678});
    ITCH_CHECK_EQ(load_be<u64>(p), u64{0x123456789ABCDEF0});
}

ITCH_TEST(endian_load_is_unaligned_safe) {
    // Same value read at four successive offsets; none of these are guaranteed
    // aligned and UBSan's alignment check is on in the sanitiser build.
    for (std::size_t off = 0; off < 4; ++off) {
        ITCH_TEST_CONTEXT("offset=" + std::to_string(off));
        const std::byte* p = kBuf.data() + off;
        const u32 v = load_be<u32>(p);
        const u32 expect = (static_cast<u32>(std::to_integer<u8>(kBuf[off])) << 24) |
                           (static_cast<u32>(std::to_integer<u8>(kBuf[off + 1])) << 16) |
                           (static_cast<u32>(std::to_integer<u8>(kBuf[off + 2])) << 8) |
                           (static_cast<u32>(std::to_integer<u8>(kBuf[off + 3])));
        ITCH_CHECK_EQ(v, expect);
    }
}

ITCH_TEST(endian_load_be48) {
    const std::byte* p = kBuf.data() + 3;
    // 0x12 34 56 78 9A BC
    ITCH_CHECK_EQ(load_be48(p), u64{0x123456789ABC});
    // Top 16 bits of the 64-bit result must be clear.
    ITCH_CHECK_EQ(load_be48(p) >> 48, u64{0});
}

ITCH_TEST(endian_load_be48_max) {
    std::array<std::byte, 6> all_ff{};
    all_ff.fill(std::byte{0xFF});
    ITCH_CHECK_EQ(load_be48(all_ff.data()), u64{0xFFFFFFFFFFFF});
}

ITCH_TEST(endian_store_load_roundtrip) {
    // Seeded so any failure replays exactly.
    constexpr u64 kSeed = 0x5EED0001;
    itch::test::Rng rng{kSeed};
    ITCH_TEST_CONTEXT("seed=" + std::to_string(kSeed));

    std::array<std::byte, 32> buf{};
    for (int iter = 0; iter < 4096; ++iter) {
        const std::size_t off = static_cast<std::size_t>(rng.range(0, 16));
        const u64 v = rng.next();

        store_be<u16>(buf.data() + off, static_cast<u16>(v));
        ITCH_REQUIRE_EQ(load_be<u16>(buf.data() + off), static_cast<u16>(v));

        store_be<u32>(buf.data() + off, static_cast<u32>(v));
        ITCH_REQUIRE_EQ(load_be<u32>(buf.data() + off), static_cast<u32>(v));

        store_be<u64>(buf.data() + off, v);
        ITCH_REQUIRE_EQ(load_be<u64>(buf.data() + off), v);

        const u64 v48 = v & 0xFFFFFFFFFFFFULL;
        store_be48(buf.data() + off, v48);
        ITCH_REQUIRE_EQ(load_be48(buf.data() + off), v48);
    }
}

ITCH_TEST(endian_store_writes_big_endian_order) {
    std::array<std::byte, 4> buf{};
    store_be<u32>(buf.data(), 0x01020304);
    ITCH_CHECK_EQ(std::to_integer<u8>(buf[0]), u8{0x01});
    ITCH_CHECK_EQ(std::to_integer<u8>(buf[1]), u8{0x02});
    ITCH_CHECK_EQ(std::to_integer<u8>(buf[2]), u8{0x03});
    ITCH_CHECK_EQ(std::to_integer<u8>(buf[3]), u8{0x04});
}

ITCH_TEST(digest_is_deterministic) {
    Digest a;
    Digest b;
    for (u64 i = 0; i < 100; ++i) {
        a.feed(i);
        b.feed(i);
    }
    ITCH_CHECK_EQ(a.value(), b.value());
}

ITCH_TEST(digest_depends_on_order) {
    Digest a;
    a.feed(1);
    a.feed(2);
    Digest b;
    b.feed(2);
    b.feed(1);
    ITCH_CHECK_NE(a.value(), b.value());
}

ITCH_TEST(digest_depends_on_value) {
    Digest a;
    a.feed(0);
    Digest b;
    b.feed(1);
    ITCH_CHECK_NE(a.value(), b.value());
}

ITCH_TEST(digest_empty_is_nonzero) {
    // A zero digest would be indistinguishable from an uninitialised one.
    ITCH_CHECK_NE(Digest{}.value(), u64{0});
}

ITCH_TEST(digest_reset_restores_initial_state) {
    Digest a;
    const u64 initial = a.value();
    a.feed(42);
    a.reset();
    ITCH_CHECK_EQ(a.value(), initial);
}

ITCH_TEST(digest_is_a_compile_time_constant) {
    // Guards against the digest picking up anything address- or build-dependent.
    constexpr u64 kExpect = [] {
        Digest d;
        d.feed(1);
        d.feed(2);
        d.feed(3);
        return d.value();
    }();
    Digest runtime;
    runtime.feed(1);
    runtime.feed(2);
    runtime.feed(3);
    ITCH_CHECK_EQ(runtime.value(), kExpect);
}

ITCH_TEST(digest_str_length_is_significant) {
    Digest a;
    a.feed_str("ab");
    a.feed_str("c");
    Digest b;
    b.feed_str("a");
    b.feed_str("bc");
    ITCH_CHECK_NE(a.value(), b.value());
}

ITCH_TEST(types_side_opposite) {
    ITCH_CHECK(opposite(Side::Buy) == Side::Sell);
    ITCH_CHECK(opposite(Side::Sell) == Side::Buy);
    ITCH_CHECK_EQ(to_char(Side::Buy), 'B');
    ITCH_CHECK_EQ(to_char(Side::Sell), 'S');
}

ITCH_TEST(types_ticks_ordering) {
    ITCH_CHECK(Ticks{1} < Ticks{2});
    ITCH_CHECK(Ticks{-1} < Ticks{0});
    ITCH_CHECK(Ticks{5} == Ticks{5});
}

ITCH_TEST(types_price_scale) {
    // $12.34 in raw ITCH units.
    constexpr Price kTwelveThirtyFour = 123400;
    ITCH_CHECK_EQ(kTwelveThirtyFour / kPriceScale, Price{12});
    ITCH_CHECK_EQ(kTwelveThirtyFour % kPriceScale, Price{3400});
    ITCH_CHECK_EQ(kTwelveThirtyFour % kPennyUnits, Price{0});
}

ITCH_TEST(assert_fires_on_false) {
    ITCH_REQUIRE_ASSERT(ITCH_ASSERT(1 == 2));
}

ITCH_TEST(assert_does_not_fire_on_true) {
    // If this trips, the whole assertion mechanism is inverted.
    ITCH_ASSERT(1 == 1);
    ITCH_ASSERT_MSG(true, "should not fire");
}

ITCH_TEST(assert_handler_is_restored_after_trap) {
    ITCH_REQUIRE_ASSERT(ITCH_ASSERT(false));
    // Nested trap must work after the first one has been torn down.
    ITCH_REQUIRE_ASSERT(ITCH_ASSERT(false));
}
