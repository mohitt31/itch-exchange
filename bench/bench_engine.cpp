// Matching engine throughput.
//
// The engine is a different shape of work from replay and needs its own
// workload. Replaying ITCH applies decisions NASDAQ already made; the engine
// makes them, so what it costs depends on how often an incoming order crosses
// and how deep it has to sweep when it does. A stream of orders that never
// cross measures the book's add path and calls it matching.
//
// So the flow is parameterised by how aggressive it is, and the report says how
// many fills each setting actually produced. A number without that is not
// interpretable.

#include <cstdio>
#include <string>
#include <vector>

#include "bench_support.hpp"
#include "itch/book/flat_book.hpp"
#include "itch/engine/matching_engine.hpp"

using namespace itch;
using namespace itch::bench;
using namespace itch::book;
using namespace itch::engine;

namespace {

constexpr Price kMid = 100'0000;
constexpr Price kTick = 100;

struct Flow {
    std::vector<NewOrder> orders;
    const char*           name;
};

// Builds the flow in an untimed pass against a live book.
//
// "Aggressive" means priced through the opposite touch, and the touch only
// exists once there is a book -- so it cannot be decided from a precomputed
// drifting mid. The first version did exactly that, and its 0% aggressive flow
// produced 289,108 fills: a bid placed when the mid was high crosses an offer
// placed after it fell. Every row measured the same thing and the parameter
// controlled nothing.
//
// Here the price is computed from the book's actual touch at the moment the
// order is submitted, then recorded. The timed pass replays the recorded prices,
// so it reproduces the same matching with no generator work inside the loop.
Flow make_flow(const char* name, u32 aggressive_pct, std::size_t n, u64 seed) {
    Flow f{{}, name};
    f.orders.reserve(n);
    Rng rng{seed};

    const auto cap = static_cast<u32>(n);
    FlatBook   book{cap + 1024, 1u << 16, index_entries_for(cap)};
    MatchingEngine<FlatBook> engine{book, StpMode::CancelNewest};
    std::vector<Fill>        fills;

    i64 mid = kMid / kTick;
    for (std::size_t i = 0; i < n; ++i) {
        mid += static_cast<i64>(rng.range(0, 6)) - 3;
        NewOrder o;
        o.client_id = i + 1;
        o.owner = static_cast<u16>(rng.range(1, 8));
        o.side = rng.chance(50) ? Side::Buy : Side::Sell;
        o.type = OrderType::Limit;
        o.tif = TimeInForce::Day;
        o.qty = static_cast<Qty>(rng.range(1, 600));

        const Side other = itch::opposite(o.side);
        const i64  off = static_cast<i64>(rng.range(1, 25));
        const bool aggressive = rng.chance(aggressive_pct);

        if (aggressive && !book.empty(other)) {
            // Through the touch by a few ticks, so it crosses and may sweep.
            const i64 touch = static_cast<i64>(book.best(other)) / kTick;
            const i64 depth = static_cast<i64>(rng.range(0, 4));
            mid = touch;
            o.price = static_cast<Price>(
                (o.side == Side::Buy ? touch + depth : touch - depth) * kTick);
        } else if (!book.empty(other)) {
            // Behind the touch, so it rests. Anchored on the touch rather than
            // on a free-running mid, which is what makes "rests" true.
            const i64 touch = static_cast<i64>(book.best(other)) / kTick;
            o.price = static_cast<Price>(
                (o.side == Side::Buy ? touch - off : touch + off) * kTick);
        } else {
            o.price = static_cast<Price>(
                (o.side == Side::Buy ? mid - off : mid + off) * kTick);
        }
        if (o.price < kTick) {
            o.price = kTick;
        }

        fills.clear();
        (void)engine.submit(o, fills);
        f.orders.push_back(o);
    }
    return f;
}

struct Result {
    double ops_per_sec = 0;
    u64    fills = 0;
    u64    filled_qty = 0;
    u64    rested = 0;
};

Result run_once(const Flow& f) {
    // Sized for the worst case this benchmark can produce: the 0% aggressive
    // flow never trades, so every order rests and the book has to hold all of
    // them. That flow is not realistic -- a market where nothing crosses is not
    // a market -- but it is the control that says what the pure add path costs,
    // so it has to be runnable.
    const auto n = static_cast<u32>(f.orders.size());
    FlatBook   book{n + 1024, 1u << 16, index_entries_for(n)};
    MatchingEngine<FlatBook> engine{book, StpMode::CancelNewest};
    std::vector<Fill>    fills;
    fills.reserve(64);

    // Accumulates filled AND resting quantity. Filled alone was zero for the
    // 0% aggressive flow, which never trades by construction, so the dead-code
    // guard fired on a legitimate configuration. Every order either fills or
    // rests, so this is non-zero for any flow that did any work at all.
    u64       sink = 0;
    const u64 t0 = now_ns();
    for (const NewOrder& o : f.orders) {
        fills.clear();
        const Report r = engine.submit(o, fills);
        sink += r.filled + r.resting_qty;
    }
    const u64 t1 = now_ns();
    do_not_optimize(sink);
    if (sink == 0) {
        std::fprintf(stderr, "impossible: nothing filled and nothing rested\n");
        std::exit(1);
    }

    Result res;
    res.ops_per_sec =
        static_cast<double>(f.orders.size()) * 1e9 / static_cast<double>(t1 - t0);
    res.fills = engine.stats().fills;
    res.filled_qty = engine.stats().filled_qty;
    res.rested = engine.stats().rested;
    return res;
}

}  // namespace

int main(int argc, char** argv) {
    int         runs = 5;
    std::size_t n = 2'000'000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--runs" && i + 1 < argc) {
            runs = std::atoi(argv[++i]);
        } else if (a == "--orders" && i + 1 < argc) {
            n = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        }
    }

    std::printf("orders per round   %s\n", commas(n).c_str());
    std::printf("self-trade mode    cancel newest, 8 participants\n");
    std::printf("median of %d rounds\n\n", runs);

    const Flow flows[] = {
        make_flow("0% aggressive", 0, n, 0x5EED0100),
        make_flow("5% aggressive", 5, n, 0x5EED0101),
        make_flow("25% aggressive", 25, n, 0x5EED0102),
        make_flow("60% aggressive", 60, n, 0x5EED0103),
    };

    std::printf("%-16s %14s %10s %14s %14s\n", "flow", "orders/s", "ns/order", "fills",
                "rested");
    std::printf("%s\n", std::string(72, '-').c_str());

    for (const Flow& f : flows) {
        do_not_optimize(run_once(f).ops_per_sec);  // warmup
        std::vector<double> rounds;
        Result              last;
        for (int r = 0; r < runs; ++r) {
            last = run_once(f);
            rounds.push_back(last.ops_per_sec);
        }
        const double m = median_of(std::move(rounds));
        std::printf("%-16s %14s %10.1f %14s %14s\n", f.name,
                    commas(static_cast<u64>(m)).c_str(), 1e9 / m,
                    commas(last.fills).c_str(), commas(last.rested).c_str());
    }
    std::printf("\nfills and rested are for one round, so the cost of a row can be read\n");
    std::printf("against how much matching it actually did.\n");
    return 0;
}
