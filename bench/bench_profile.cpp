// A long-running FlatBook workload, for profiling, plus the book's own counters.
//
// Two ways of finding a bottleneck, used together. The counters say what the
// structure is actually doing -- how many probes per index lookup, how often the
// best price has to be recomputed, how much traffic misses the ladder window --
// which points at a cause. The sampler says where the time goes, which points
// at a location. Neither alone is enough: a hot function with a good reason to
// be hot is not a bottleneck, and a bad counter in cold code is not either.
//
// Usage:
//   bench_profile [--symbol SYM] [--seconds N] <corpus>
//   sample <pid> 10 -file /tmp/prof.txt      (while it runs)

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "bench_support.hpp"
#include "itch/book/flat_book.hpp"
#include "workload.hpp"

using namespace itch;
using namespace itch::bench;
using itch::book::FlatBook;
using itch::book::Side;

int main(int argc, char** argv) {
    std::string corpus;
    std::string symbol = "QQQ";
    double      seconds = 10.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--symbol" && i + 1 < argc) {
            symbol = argv[++i];
        } else if (a == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else {
            corpus = a;
        }
    }
    if (corpus.empty()) {
        std::fprintf(stderr, "usage: bench_profile [--symbol SYM] [--seconds N] <corpus>\n");
        return 2;
    }

    const Workload w = load_workload(corpus, symbol);
    if (w.ops.empty()) {
        std::fprintf(stderr, "error: no operations for %s\n", symbol.c_str());
        return 1;
    }
    std::printf("pid %d, %s operations, running %.0f s\n", static_cast<int>(::getpid()),
                commas(w.ops.size()).c_str(), seconds);
    std::fflush(stdout);

    u64  passes = 0;
    u64  digest = 0;
    const auto t0 = std::chrono::steady_clock::now();
    FlatBook last;
    for (;;) {
        FlatBook book;
        for (const WorkloadOp& op : w.ops) {
            apply(book, op);
        }
        digest = itch::book::book_digest(book);
        do_not_optimize(digest);
        ++passes;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >=
            seconds) {
            // Rebuild one last book so its counters survive the loop.
            for (const WorkloadOp& op : w.ops) {
                apply(last, op);
            }
            break;
        }
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("\npasses           %llu in %.2f s (%s ops/s)\n",
                static_cast<unsigned long long>(passes), elapsed,
                commas(static_cast<u64>(static_cast<double>(passes * w.ops.size()) /
                                        elapsed))
                    .c_str());
    std::printf("digest           %016llx\n", static_cast<unsigned long long>(digest));

    std::printf("\nwhat the structure did over one pass\n");
    std::printf("  order index\n");
    std::printf("    inserts        %s\n", commas(last.index().inserts()).c_str());
    std::printf("    extra probes   %s\n", commas(last.index().probes()).c_str());
    std::printf("    probes per op  %.4f\n",
                last.index().inserts() == 0
                    ? 0.0
                    : static_cast<double>(last.index().probes()) /
                          static_cast<double>(last.index().inserts()));
    std::printf("    capacity       %s (live %s)\n",
                commas(last.index().capacity()).c_str(),
                commas(last.index().size()).c_str());
    std::printf("  best price cache\n");
    std::printf("    recomputes     %s (%.2f%% of operations)\n",
                commas(last.best_recomputes()).c_str(),
                100.0 * static_cast<double>(last.best_recomputes()) /
                    static_cast<double>(w.ops.size()));
    for (Side s : {Side::Buy, Side::Sell}) {
        const auto& lad = last.ladder(s);
        std::printf("  ladder %s\n", s == Side::Buy ? "bid" : "ask");
        std::printf("    window hits    %s\n", commas(lad.window_hits()).c_str());
        std::printf("    overflow hits  %s (%.4f%%)\n", commas(lad.overflow_hits()).c_str(),
                    100.0 * static_cast<double>(lad.overflow_hits()) /
                        static_cast<double>(lad.window_hits() + lad.overflow_hits() + 1));
        std::printf("    rebases        %s\n", commas(lad.rebases()).c_str());
        std::printf("    displaced      %s\n", commas(lad.levels_displaced()).c_str());
        std::printf("    live levels    %s (overflow %s)\n", commas(lad.size()).c_str(),
                    commas(lad.overflow_count()).c_str());
    }
    std::printf("  pools\n");
    std::printf("    order peak     %s\n", commas(last.order_high_water()).c_str());
    std::printf("    level peak     %s\n", commas(last.level_high_water()).c_str());
    return 0;
}
