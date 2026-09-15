// The gzip path must produce byte-identical frames to the plain path, and must
// distinguish a truncated stream from a corrupt one.
//
// Both of those were broken when this file did not exist. A z_stream cannot be
// moved by value, because zlib's internal state holds a back-pointer to the
// z_stream it was initialised with; moving it made every inflate call return
// Z_STREAM_ERROR. The error was invisible because the read loop treated any
// non-OK return as "truncated, stop here", so a completely dead decompressor
// looked exactly like a clean empty file.

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <zlib.h>

#include "harness.hpp"
#include "itch/wire/framing.hpp"
#include "itch/wire/gzip_source.hpp"
#include "itch/wire/source.hpp"
#include "wire_builder.hpp"

using namespace itch;
using namespace itch::wire;
using itch::test::WireBuilder;

namespace {

std::vector<std::byte> zlib_compress(std::span<const std::byte> in) {
    ::uLongf bound = ::compressBound(static_cast<::uLong>(in.size()));
    std::vector<std::byte> out(bound);
    const int rc = ::compress2(reinterpret_cast<::Bytef*>(out.data()), &bound,
                               reinterpret_cast<const ::Bytef*>(in.data()),
                               static_cast<::uLong>(in.size()), 6);
    ITCH_ASSERT(rc == Z_OK);
    out.resize(bound);
    return out;
}

std::vector<char> all_types() {
    std::vector<char> t;
    for (int c = 0; c < 256; ++c) {
        if (is_known_type(static_cast<char>(c))) {
            t.push_back(static_cast<char>(c));
        }
    }
    return t;
}

// A stream big enough to span many inflate calls and many buffer refills.
WireBuilder make_stream(u64 seed, int count) {
    WireBuilder b;
    itch::test::Rng rng{seed};
    const std::vector<char> types = all_types();
    for (int i = 0; i < count; ++i) {
        const char t = types[rng.range(0, types.size() - 1)];
        const std::size_t m = b.begin(t);
        b.put<u16>(m, 1, static_cast<u16>(rng.range(1, 9000)));
        b.put_be48(m, 5, rng.range(0, 86'400'000'000'000ULL));
    }
    return b;
}

std::vector<std::vector<std::byte>> frames_of_plain(std::span<const std::byte> buf) {
    std::vector<std::vector<std::byte>> out;
    FrameCursor cur{buf};
    std::span<const std::byte> f;
    while (cur.next(f) == FrameStatus::Ok) {
        out.emplace_back(f.begin(), f.end());
    }
    return out;
}

}  // namespace

ITCH_TEST(gzip_matches_the_plain_path_exactly) {
    constexpr u64 kSeed = 0x5EED0004;
    ITCH_TEST_CONTEXT("seed=" + std::to_string(kSeed));

    const WireBuilder b = make_stream(kSeed, 20000);
    const auto expected = frames_of_plain(b.span());
    ITCH_REQUIRE_EQ(expected.size(), std::size_t{20000});

    const std::vector<std::byte> compressed = zlib_compress(b.span());
    // Must actually be compressing, otherwise this proves much less.
    ITCH_CHECK_LT(compressed.size(), b.size());

    GzipSource<MemorySource> gz{MemorySource{compressed, 997}};
    FrameReader<GzipSource<MemorySource>> reader{std::move(gz), kMinBufferSize};

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
    ITCH_CHECK_EQ(reader.bytes_consumed(), static_cast<u64>(b.size()));
}

ITCH_TEST(gzip_survives_being_moved) {
    // Moving the source is how every caller builds a FrameReader. A z_stream
    // moved by value is rejected by zlib on the next inflate call.
    const WireBuilder b = make_stream(0x5EED0005, 500);
    const std::vector<std::byte> compressed = zlib_compress(b.span());

    GzipSource<MemorySource> a{MemorySource{compressed}};
    GzipSource<MemorySource> moved{std::move(a)};

    std::vector<std::byte> out(1 << 16);
    const std::size_t got = moved.read(out.data(), out.size());
    ITCH_REQUIRE_GT(got, std::size_t{0});
    ITCH_CHECK_EQ(std::memcmp(out.data(), b.span().data(), got), 0);
}

ITCH_TEST(gzip_truncated_input_is_a_clean_stop) {
    // The corpus is a byte range of a gzip stream, so it always ends like this.
    const WireBuilder b = make_stream(0x5EED0006, 5000);
    std::vector<std::byte> compressed = zlib_compress(b.span());
    compressed.resize(compressed.size() / 2);

    GzipSource<MemorySource> gz{MemorySource{compressed}, /*allow_truncated=*/true};
    FrameReader<GzipSource<MemorySource>> reader{std::move(gz), kMinBufferSize};

    std::span<const std::byte> f;
    std::size_t n = 0;
    FrameStatus last = FrameStatus::Ok;
    for (;;) {
        last = reader.next(f);
        if (last != FrameStatus::Ok) {
            break;
        }
        ++n;
    }
    // Some messages must survive: a truncated prefix is still useful data.
    ITCH_CHECK_GT(n, std::size_t{100});
    ITCH_CHECK_LT(n, std::size_t{5000});
    ITCH_CHECK(last == FrameStatus::EndOfInput || last == FrameStatus::NeedMore);
}

ITCH_TEST(gzip_truncated_input_throws_when_not_allowed) {
    const WireBuilder b = make_stream(0x5EED0007, 5000);
    std::vector<std::byte> compressed = zlib_compress(b.span());
    compressed.resize(compressed.size() / 2);

    GzipSource<MemorySource> gz{MemorySource{compressed}, /*allow_truncated=*/false};
    std::vector<std::byte> out(1 << 20);
    bool threw = false;
    try {
        while (gz.read(out.data(), out.size()) != 0) {
        }
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ITCH_CHECK(threw);
}

ITCH_TEST(gzip_corrupt_input_always_throws) {
    // allow_truncated forgives a short stream, never a broken one. Reporting a
    // corrupt file as an empty one is how the z_stream bug stayed hidden.
    const WireBuilder b = make_stream(0x5EED0008, 5000);
    std::vector<std::byte> compressed = zlib_compress(b.span());
    ITCH_REQUIRE_GT(compressed.size(), std::size_t{200});

    // Damage the middle of the deflate data, past the header.
    for (std::size_t i = compressed.size() / 2; i < compressed.size() / 2 + 64; ++i) {
        compressed[i] = static_cast<std::byte>(~std::to_integer<u8>(compressed[i]));
    }

    GzipSource<MemorySource> gz{MemorySource{compressed}, /*allow_truncated=*/true};
    std::vector<std::byte> out(1 << 16);
    bool threw = false;
    try {
        while (gz.read(out.data(), out.size()) != 0) {
        }
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ITCH_CHECK(threw);
}

ITCH_TEST(gzip_bad_header_throws_immediately) {
    std::vector<std::byte> garbage(4096);
    for (std::size_t i = 0; i < garbage.size(); ++i) {
        garbage[i] = static_cast<std::byte>(i * 31 + 7);
    }
    GzipSource<MemorySource> gz{MemorySource{garbage}, /*allow_truncated=*/true};
    std::vector<std::byte> out(1024);
    bool threw = false;
    try {
        gz.read(out.data(), out.size());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ITCH_CHECK(threw);
}
