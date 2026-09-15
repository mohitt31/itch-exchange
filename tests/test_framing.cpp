#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "harness.hpp"
#include "itch/wire/framing.hpp"
#include "itch/wire/source.hpp"
#include "wire_builder.hpp"

using namespace itch;
using namespace itch::wire;
using itch::test::WireBuilder;

namespace {

// Builds a stream containing one message of every known type, in spec order.
std::vector<char> kAllTypes() {
    std::vector<char> t;
    for (int c = 0; c < 256; ++c) {
        if (is_known_type(static_cast<char>(c))) {
            t.push_back(static_cast<char>(c));
        }
    }
    return t;
}

std::vector<std::span<const std::byte>> collect(std::span<const std::byte> buf,
                                                FrameStatus& end, FrameError& err) {
    std::vector<std::span<const std::byte>> frames;
    FrameCursor cur{buf};
    std::span<const std::byte> f;
    for (;;) {
        const FrameStatus st = cur.next(f);
        if (st != FrameStatus::Ok) {
            end = st;
            err = cur.error();
            return frames;
        }
        frames.push_back(f);
    }
}

}  // namespace

ITCH_TEST(framing_empty_input_is_end_of_input) {
    FrameCursor cur{std::span<const std::byte>{}};
    std::span<const std::byte> f;
    ITCH_CHECK(cur.next(f) == FrameStatus::EndOfInput);
    ITCH_CHECK(cur.error() == FrameError::None);
    ITCH_CHECK_EQ(cur.offset(), std::size_t{0});
}

ITCH_TEST(framing_reads_one_message_of_every_type) {
    WireBuilder b;
    const std::vector<char> types = kAllTypes();
    for (char t : types) {
        b.begin(t);
    }

    FrameStatus end{};
    FrameError  err{};
    const auto frames = collect(b.span(), end, err);

    ITCH_REQUIRE(end == FrameStatus::EndOfInput);
    ITCH_REQUIRE_EQ(frames.size(), types.size());
    ITCH_REQUIRE_EQ(frames.size(), kMessageTypeCount);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        ITCH_TEST_CONTEXT(std::string("type='") + types[i] + "'");
        ITCH_CHECK_EQ(static_cast<char>(frames[i][0]), types[i]);
        ITCH_CHECK_EQ(frames[i].size(), message_length(types[i]));
    }
}

ITCH_TEST(framing_offset_accounts_for_every_byte) {
    WireBuilder b;
    for (char t : kAllTypes()) {
        b.begin(t);
    }
    FrameCursor cur{b.span()};
    std::span<const std::byte> f;
    while (cur.next(f) == FrameStatus::Ok) {
    }
    // Nothing may be left over on a well-formed stream.
    ITCH_CHECK_EQ(cur.offset(), b.size());
    ITCH_CHECK(cur.rest().empty());
}

ITCH_TEST(framing_rejects_zero_length) {
    WireBuilder b;
    b.append_raw(0, {});
    FrameStatus end{};
    FrameError  err{};
    const auto frames = collect(b.span(), end, err);
    ITCH_CHECK_EQ(frames.size(), std::size_t{0});
    ITCH_CHECK(end == FrameStatus::Malformed);
    ITCH_CHECK(err == FrameError::ZeroLength);
}

ITCH_TEST(framing_rejects_unknown_type) {
    // Well formed framing, but 'G' is not an ITCH 5.0 message.
    const std::array<std::byte, 4> body = {std::byte{'G'}, std::byte{0}, std::byte{0},
                                           std::byte{0}};
    WireBuilder b;
    b.append_raw(4, body);
    FrameStatus end{};
    FrameError  err{};
    const auto frames = collect(b.span(), end, err);
    ITCH_CHECK_EQ(frames.size(), std::size_t{0});
    ITCH_CHECK(end == FrameStatus::Malformed);
    ITCH_CHECK(err == FrameError::UnknownType);
}

ITCH_TEST(framing_rejects_length_that_disagrees_with_type) {
    // 'A' is 36 bytes. Declaring 35 must be caught, not followed: this is the
    // check that makes a run over the real feed test all 23 layout structs.
    std::array<std::byte, 35> body{};
    body[0] = std::byte{'A'};
    WireBuilder b;
    b.append_raw(35, body);
    FrameStatus end{};
    FrameError  err{};
    const auto frames = collect(b.span(), end, err);
    ITCH_CHECK_EQ(frames.size(), std::size_t{0});
    ITCH_CHECK(end == FrameStatus::Malformed);
    ITCH_CHECK(err == FrameError::LengthMismatch);
}

ITCH_TEST(framing_detects_a_length_mismatch_in_every_direction) {
    for (char t : kAllTypes()) {
        for (int delta : {-1, 1}) {
            ITCH_TEST_CONTEXT(std::string("type='") + t + "' delta=" +
                              std::to_string(delta));
            const std::size_t real = message_length(t);
            const std::size_t bad =
                static_cast<std::size_t>(static_cast<long>(real) + delta);
            std::vector<std::byte> body(bad, std::byte{0});
            body[0] = static_cast<std::byte>(t);
            WireBuilder b;
            b.append_raw(static_cast<u16>(bad), body);
            FrameStatus end{};
            FrameError  err{};
            const auto frames = collect(b.span(), end, err);
            ITCH_REQUIRE_EQ(frames.size(), std::size_t{0});
            ITCH_REQUIRE(end == FrameStatus::Malformed);
            ITCH_REQUIRE(err == FrameError::LengthMismatch);
        }
    }
}

ITCH_TEST(framing_reports_a_truncated_length_prefix) {
    WireBuilder b;
    b.begin('D');
    b.truncate(b.size() - 1);  // a single stray byte
    FrameStatus end{};
    FrameError  err{};
    const auto frames = collect(b.span(), end, err);
    ITCH_CHECK_EQ(frames.size(), std::size_t{0});
    ITCH_CHECK(end == FrameStatus::NeedMore);
    ITCH_CHECK(err == FrameError::TruncatedLengthPrefix);
}

ITCH_TEST(framing_reports_a_truncated_body) {
    // Two good messages then a cut one. The good ones must still come out: a
    // gzip prefix ends exactly like this and the whole corpus depends on it.
    WireBuilder b;
    b.begin('D');
    b.begin('A');
    b.begin('U');
    b.truncate(10);

    FrameStatus end{};
    FrameError  err{};
    const auto frames = collect(b.span(), end, err);
    ITCH_REQUIRE_EQ(frames.size(), std::size_t{2});
    ITCH_CHECK_EQ(static_cast<char>(frames[0][0]), 'D');
    ITCH_CHECK_EQ(static_cast<char>(frames[1][0]), 'A');
    ITCH_CHECK(end == FrameStatus::NeedMore);
    ITCH_CHECK(err == FrameError::TruncatedBody);
}

ITCH_TEST(framing_does_not_consume_a_malformed_frame) {
    WireBuilder b;
    b.begin('D');
    std::array<std::byte, 4> bad = {std::byte{'G'}, std::byte{0}, std::byte{0},
                                    std::byte{0}};
    b.append_raw(4, bad);

    FrameCursor cur{b.span()};
    std::span<const std::byte> f;
    ITCH_REQUIRE(cur.next(f) == FrameStatus::Ok);
    const std::size_t after_good = cur.offset();
    ITCH_REQUIRE(cur.next(f) == FrameStatus::Malformed);
    // The cursor must sit on the bad frame so it can be reported, not skipped.
    ITCH_CHECK_EQ(cur.offset(), after_good);
    ITCH_CHECK_EQ(cur.rest().size(), std::size_t{6});
}

// --- buffered path -------------------------------------------------------

ITCH_TEST(reader_rejects_a_buffer_too_small_for_one_message) {
    WireBuilder b;
    b.begin('A');
    ITCH_REQUIRE_ASSERT(
        (FrameReader<MemorySource>{MemorySource{b.span()}, kMinBufferSize - 1}));
}

ITCH_TEST(reader_matches_the_zero_copy_path) {
    // Same input through both paths, byte for byte. The chunk sizes are chosen
    // to be smaller than a message so the straddle handling is forced.
    WireBuilder b;
    itch::test::Rng rng{0x5EED0003};
    const std::vector<char> types = kAllTypes();
    for (int i = 0; i < 2000; ++i) {
        b.begin(types[rng.range(0, types.size() - 1)]);
    }

    FrameStatus end{};
    FrameError  err{};
    const auto expected = collect(b.span(), end, err);
    ITCH_REQUIRE(end == FrameStatus::EndOfInput);

    for (std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{7},
                              std::size_t{64}, std::size_t{4096}}) {
        ITCH_TEST_CONTEXT("chunk=" + std::to_string(chunk));
        FrameReader<MemorySource> reader{MemorySource{b.span(), chunk}, kMinBufferSize};
        std::span<const std::byte> f;
        std::size_t i = 0;
        for (;;) {
            const FrameStatus st = reader.next(f);
            if (st != FrameStatus::Ok) {
                ITCH_REQUIRE(st == FrameStatus::EndOfInput);
                break;
            }
            ITCH_REQUIRE_LT(i, expected.size());
            ITCH_REQUIRE_EQ(f.size(), expected[i].size());
            ITCH_REQUIRE_EQ(std::memcmp(f.data(), expected[i].data(), f.size()), 0);
            ++i;
        }
        ITCH_CHECK_EQ(i, expected.size());
        ITCH_CHECK_EQ(reader.frames(), static_cast<u64>(expected.size()));
        ITCH_CHECK_EQ(reader.bytes_consumed(), static_cast<u64>(b.size()));
        ITCH_CHECK_EQ(reader.trailing_bytes(), std::size_t{0});
    }
}

ITCH_TEST(reader_reports_a_truncated_tail) {
    WireBuilder b;
    b.begin('A');
    b.begin('P');
    b.truncate(20);

    FrameReader<MemorySource> reader{MemorySource{b.span(), 5}, kMinBufferSize};
    std::span<const std::byte> f;
    ITCH_REQUIRE(reader.next(f) == FrameStatus::Ok);
    ITCH_CHECK_EQ(static_cast<char>(f[0]), 'A');
    ITCH_CHECK(reader.next(f) == FrameStatus::NeedMore);
    ITCH_CHECK(reader.error() == FrameError::TruncatedBody);
    ITCH_CHECK_GT(reader.trailing_bytes(), std::size_t{0});
}

ITCH_TEST(reader_propagates_malformed) {
    WireBuilder b;
    b.begin('D');
    b.append_raw(0, {});
    FrameReader<MemorySource> reader{MemorySource{b.span(), 4}, kMinBufferSize};
    std::span<const std::byte> f;
    ITCH_REQUIRE(reader.next(f) == FrameStatus::Ok);
    ITCH_CHECK(reader.next(f) == FrameStatus::Malformed);
    ITCH_CHECK(reader.error() == FrameError::ZeroLength);
}
