// What the ladder's two paths actually cost.
//
// DESIGN.md claims the flat window is O(1) and the overflow map is the slow
// fallback, and measures how much traffic takes each. It did not measure how
// much slower the fallback is, which is the other half of the claim: if the two
// paths cost the same, the window is not buying anything and the hit rate is
// irrelevant.
//
// Both paths are driven with the same number of lookups against the same
// populated ladder, differing only in whether the price is inside the window.

#include <cstdio>
#include <vector>

#include "bench_support.hpp"
#include "itch/book/price_ladder.hpp"

using namespace itch;
using namespace itch::bench;
using namespace itch::book;

namespace {

constexpr Price kPenny = 100;

// $100,000.00 in raw ITCH units. Deliberately high: the far levels walk
// downwards from here, and at $100.00 they underflow Price, wrap to enormous
// values, become the best bid and drag the window off the near levels -- which
// made the window path appear to slow down as the overflow grew. The
// assertion below catches that class of harness bug rather than trusting this
// comment.
constexpr Price kBase = 1'000'000'000;
constexpr int   kLookups = 20'000'000;

LevelHandle h(u32 i) { return LevelHandle::make(i, 1); }

// A ladder holding `in_window` levels near the base and `outside` levels far
// away, matching the measured shape: a dense core plus stub quotes and deep
// levels in the overflow map.
PriceLadder build(u32 in_window, u32 outside, std::vector<Price>& near,
                  std::vector<Price>& far) {
    PriceLadder lad{Side::Buy};
    u32         next = 1;
    for (u32 i = 0; i < in_window; ++i) {
        const Price p = kBase - static_cast<Price>(i) * kPenny;
        lad.insert(p, h(next++));
        near.push_back(p);
    }
    // Far prices: beyond +/- 2048 ticks, so they live in the overflow map.
    for (u32 i = 0; i < outside; ++i) {
        const Price p = kBase - static_cast<Price>(3000 + i * 37) * kPenny;
        lad.insert(p, h(next++));
        far.push_back(p);
    }
    return lad;
}

// The price sequence is precomputed and indexed with a mask, never a modulo.
// The first version of this used `i % prices.size()`, and an integer division
// costs more than either lookup path being compared -- both came out at about
// 9 ns and the ratio collapsed to 1.24x, which says nothing about the ladder.
double time_lookups(const PriceLadder& lad, const std::vector<Price>& seq) {
    const std::size_t mask = seq.size() - 1;
    u64               acc = 0;
    const u64         t0 = now_ns();
    for (int i = 0; i < kLookups; ++i) {
        acc += lad.find(seq[static_cast<std::size_t>(i) & mask]).bits();
    }
    const u64 t1 = now_ns();
    do_not_optimize(acc);
    if (acc == 0) {
        std::fprintf(stderr, "impossible: every lookup missed\n");
        std::exit(1);
    }
    return static_cast<double>(t1 - t0) / kLookups;
}

// Power-of-two length, so the index is a mask.
std::vector<Price> cycle_of(const std::vector<Price>& src, std::size_t n) {
    std::vector<Price> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = src[i % src.size()];
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    print_power_state();

    int runs = 5;
    if (argc > 2 && std::string(argv[1]) == "--runs") {
        runs = std::atoi(argv[2]);
    }

    // The window holds 1,277 levels throughout -- the measured QQQ peak. Only
    // the overflow population is swept, because the point of the flat window is
    // that its cost does not depend on how much is in it and a tree's does.
    std::printf("levels in window   1,277 (the measured QQQ peak) in every row\n");
    std::printf("lookups per round  %s\n\n", commas(kLookups).c_str());
    std::printf("%12s %16s %16s %10s\n", "overflow", "window ns", "overflow ns",
                "slower by");
    std::printf("%s\n", std::string(58, '-').c_str());

    for (u32 outside : {u32{64}, u32{544}, u32{4096}, u32{32768}, u32{262144}}) {
        std::vector<Price> near, far;
        const PriceLadder  lad = build(1277, outside, near, far);

        // The whole comparison depends on these landing where they are meant
        // to. Check it, do not assume it.
        if (lad.in_window_count() != near.size() ||
            lad.overflow_count() != far.size()) {
            std::fprintf(stderr,
                         "harness error at outside=%u: %u in window (want %zu), "
                         "%zu in overflow (want %zu)\n",
                         outside, lad.in_window_count(), near.size(),
                         lad.overflow_count(), far.size());
            return 1;
        }

        const auto near_seq = cycle_of(near, 1024);
        const auto far_seq = cycle_of(far, 1024);

        // NOT interleaved here, unlike the book benchmark. Walking a 262,144
        // node std::map evicts the window's 16 KiB array and 512 B bitset, so
        // alternating the two made the window path appear to get slower as the
        // overflow grew -- which is impossible, since the window holds the same
        // 1,277 levels in every row. Each path is warmed immediately before its
        // own timed group instead.
        std::vector<double> w, o;
        do_not_optimize(time_lookups(lad, near_seq));
        for (int r = 0; r < runs; ++r) {
            w.push_back(time_lookups(lad, near_seq));
        }
        do_not_optimize(time_lookups(lad, far_seq));
        for (int r = 0; r < runs; ++r) {
            o.push_back(time_lookups(lad, far_seq));
        }
        const double wn = median_of(w);
        const double on = median_of(o);
        std::printf("%12s %16.2f %16.2f %9.2fx\n", commas(outside).c_str(), wn, on,
                    on / wn);
    }
    std::printf("\n544 is the measured overflow population for QQQ. The rows above and\n");
    std::printf("below it are there to show which path's cost depends on that number.\n");
    return 0;
}
