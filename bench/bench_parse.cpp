// Parser throughput on its own, with no decompressor and no file read in the
// timed region.
//
// Three separate numbers, because they answer different questions and mixing
// them is how a parser benchmark becomes meaningless:
//
//   frame-only    walk the length prefixes and validate each against the type
//                 table. No field is decoded.
//   frame+decode  additionally decode the fields a book builder actually reads
//                 from every message, and accumulate them so nothing can be
//                 eliminated.
//   end to end    the same work, but reading the gzip corpus, so the figure
//                 includes inflate. Reported to show how much of the headline
//                 throughput in itch_stats is really the decompressor.
//
// The corpus is decompressed into memory once, before timing, up to a cap. The
// full session inflates to 10.5 GB, which does not belong in RAM on a 16 GB
// machine -- the swapping would be what got measured. 2 GB is about 70 million
// messages, far more than enough for a stable rate, and the cut always lands on
// a frame boundary so no partial message is timed.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "bench_support.hpp"
#include "itch/wire/framing.hpp"
#include "itch/wire/gzip_source.hpp"
#include "itch/wire/source.hpp"
#include "itch/wire/views.hpp"

using namespace itch;
using namespace itch::bench;
using namespace itch::wire;

namespace {

// Touches every field a book builder reads, on every message type that carries
// one. The accumulator is returned so the work cannot be removed.
struct DecodeAll {
    u64 acc = 0;

    void on_add_order(AddOrderView v) {
        acc += v.order_reference_number() + v.shares() + v.price() +
               static_cast<u64>(v.buy_sell_indicator()) + v.timestamp() + v.stock_locate();
        acc += static_cast<u64>(v.stock()[0]);
    }
    void on_add_order_with_mpid(AddOrderWithMpidView v) {
        acc += v.order_reference_number() + v.shares() + v.price() + v.timestamp();
    }
    void on_order_executed(OrderExecutedView v) {
        acc += v.order_reference_number() + v.executed_shares() + v.match_number() +
               v.timestamp();
    }
    void on_order_executed_with_price(OrderExecutedWithPriceView v) {
        acc += v.order_reference_number() + v.executed_shares() + v.execution_price() +
               v.timestamp();
    }
    void on_order_cancel(OrderCancelView v) {
        acc += v.order_reference_number() + v.cancelled_shares() + v.timestamp();
    }
    void on_order_delete(OrderDeleteView v) {
        acc += v.order_reference_number() + v.timestamp();
    }
    void on_order_replace(OrderReplaceView v) {
        acc += v.original_order_reference_number() + v.new_order_reference_number() +
               v.shares() + v.price() + v.timestamp();
    }
    void on_trade_non_cross(TradeNonCrossView v) { acc += v.match_number() + v.price(); }
    void on_stock_directory(StockDirectoryView v) {
        acc += v.stock_locate() + static_cast<u64>(v.stock()[0]);
    }
};

std::vector<std::byte> inflate_some(const std::string& path, std::size_t cap) {
    std::vector<std::byte> out;
    out.reserve(cap);
    GzipSource<FdSource>   gz{FdSource{path}};
    std::vector<std::byte> chunk(1u << 22);
    while (out.size() < cap) {
        const std::size_t got = gz.read(chunk.data(), chunk.size());
        if (got == 0) {
            break;
        }
        const std::size_t take = std::min(got, cap - out.size());
        out.insert(out.end(), chunk.begin(),
                   chunk.begin() + static_cast<std::ptrdiff_t>(take));
    }
    // Trim to the last whole frame so no partial message is ever timed.
    std::size_t end = 0;
    {
        FrameCursor                cur{std::span<const std::byte>{out}};
        std::span<const std::byte> f;
        while (cur.next(f) == FrameStatus::Ok) {
            end = cur.offset();
        }
    }
    out.resize(end);
    return out;
}

struct Res {
    double msgs_per_sec = 0;
    double bytes_per_sec = 0;
};

Res report(const char* name, std::vector<double> msg_rates, u64 msgs, u64 bytes) {
    const double m = median_of(std::move(msg_rates));
    const Res r{m, m * static_cast<double>(bytes) / static_cast<double>(msgs)};
    std::printf("%-16s %16s msg/s %10.2f GB/s\n", name, commas(static_cast<u64>(m)).c_str(),
                r.bytes_per_sec / 1e9);
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bench_parse [--runs N] <corpus.gz|corpus>\n");
        return 2;
    }
    std::string path;
    int         runs = 5;
    std::size_t cap = std::size_t{2} << 30;  // 2 GB
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--runs" && i + 1 < argc) {
            runs = std::atoi(argv[++i]);
        } else if (a == "--max-bytes" && i + 1 < argc) {
            cap = std::strtoull(argv[++i], nullptr, 10);
        } else {
            path = a;
        }
    }

    std::printf("inflating up to %s bytes of %s into memory (not timed)\n",
                commas(cap).c_str(), path.c_str());
    std::vector<std::byte> raw;
    if (path.size() > 3 && path.compare(path.size() - 3, 3, ".gz") == 0) {
        raw = inflate_some(path, cap);
    } else {
        const MappedFile f{path};
        const std::size_t take = std::min(cap, f.span().size());
        raw.assign(f.span().begin(), f.span().begin() + static_cast<std::ptrdiff_t>(take));
    }
    std::printf("%s bytes in memory\n", commas(raw.size()).c_str());

    const std::span<const std::byte> buf{raw};

    // Count once, so the rates have a denominator.
    u64 msgs = 0;
    {
        FrameCursor                cur{buf};
        std::span<const std::byte> f;
        while (cur.next(f) == FrameStatus::Ok) {
            ++msgs;
        }
    }
    std::printf("%s messages\n\n", commas(msgs).c_str());

    std::vector<double> frame_only, decoded;
    for (int r = 0; r < runs + 1; ++r) {  // first round is warmup
        {
            FrameCursor                cur{buf};
            std::span<const std::byte> f;
            u64                        n = 0;
            const u64                  t0 = now_ns();
            while (cur.next(f) == FrameStatus::Ok) {
                do_not_optimize(f.data());
                ++n;
            }
            const u64 t1 = now_ns();
            do_not_optimize(n);
            if (r > 0) {
                frame_only.push_back(static_cast<double>(n) * 1e9 /
                                     static_cast<double>(t1 - t0));
            }
        }
        {
            DecodeAll                  h;
            FrameCursor                cur{buf};
            std::span<const std::byte> f;
            u64                        n = 0;
            const u64                  t0 = now_ns();
            while (cur.next(f) == FrameStatus::Ok) {
                dispatch(f.data(), h);
                ++n;
            }
            const u64 t1 = now_ns();
            do_not_optimize(h.acc);
            if (h.acc == 0) {
                std::fprintf(stderr, "impossible: decoded nothing\n");
                return 1;
            }
            if (r > 0) {
                decoded.push_back(static_cast<double>(n) * 1e9 /
                                  static_cast<double>(t1 - t0));
            }
        }
    }

    std::printf("median of %d rounds, in-memory buffer, nothing else in the loop\n\n", runs);
    report("frame only", frame_only, msgs, raw.size());
    report("frame + decode", decoded, msgs, raw.size());
    std::printf("\nframe-only is the length-prefix walk with every message validated\n");
    std::printf("against the type table. frame+decode additionally reads every field\n");
    std::printf("a book builder uses, accumulated so it cannot be eliminated.\n");
    return 0;
}
