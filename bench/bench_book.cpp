// Three book implementations, one real workload.
//
// What is compared, and what is held constant:
//
//   FlatBook  pooled records, intrusive queues, tick-indexed sliding ladder
//   AvlBook   the same pools, the same queues, the same order index, with the
//             ladder replaced by a hand-written AVL tree over a node pool
//   MapBook   the textbook implementation: std::map of levels, std::list per
//             level, std::unordered_map for the order index
//
// FlatBook against AvlBook isolates one variable and answers "what did the
// ladder buy?". FlatBook against MapBook changes everything at once and answers
// the blunter question "what did the whole design buy over the obvious one?".
// Both are worth knowing and they are not the same number.
//
// Honesty notes that belong next to the numbers, not in a footnote:
//
//   - the timer on this machine quantises to about 41 ns and a clock read costs
//     about 17 ns, both measured at the top of every run. A book update is
//     expected in the tens of nanoseconds, so a per-operation median can be at
//     or below the floor. Where it is, it is reported as such and not as a
//     number.
//   - latency is injected open loop against a schedule fixed in advance, so an
//     operation that runs late is recorded late rather than disappearing.
//   - a "harness floor" row runs the identical pacing loop with no book
//     operation in it, so the cost of measuring is visible separately from the
//     thing measured.
//   - all three books are digested at the end of every run and required to
//     agree, so a fast wrong answer cannot win.
//   - with --only, one implementation is measured alone in its own process.
//     That matters: measuring all three in one process, interleaved, holds
//     conditions equal for the comparison but depresses every absolute, because
//     MapBook's allocations evict the other two's working sets between rounds.
//     The interleaved run is the fair comparison; the isolated run is the
//     honest absolute. Both are reported and labelled.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

#include "bench_support.hpp"
#include "itch/book/avl_level_map.hpp"
#include "itch/book/flat_book.hpp"
#include "itch/book/map_book.hpp"
#include "itch/book/price_ladder.hpp"
#include "workload.hpp"

using namespace itch;
using namespace itch::bench;
using itch::book::AvlBook;
using itch::book::FlatBook;
using itch::book::MapBook;

namespace {

struct Result {
    std::string name;
    double      ops_per_sec = 0;   // median across rounds
    double      ops_lo = 0;        // slowest round
    double      ops_hi = 0;        // fastest round
    double      ns_per_op = 0;
    u32         p50 = 0;
    u32         p99 = 0;
    u32         p999 = 0;
    u32         p9999 = 0;
    u32         max = 0;
    double      mean = 0;
    u64         digest = 0;
};

// One timed pass over the whole workload, closed loop.
//
// Construction and prefaulting are outside the timing; the digest afterwards
// both defeats dead-code elimination and proves the work happened, since a book
// that was optimised away cannot produce one.
template <class Book>
double one_pass(const Workload& w, u64& digest_out, PerfCounters* counters = nullptr) {
    Book book;
    clobber_memory();
    if (counters != nullptr) {
        counters->start();
    }
    const u64 t0 = now_ns();
    for (const WorkloadOp& op : w.ops) {
        apply(book, op);
    }
    const u64 t1 = now_ns();
    if (counters != nullptr) {
        counters->stop();
    }
    clobber_memory();

    const u64 d = itch::book::book_digest(book);
    do_not_optimize(d);
    if (d == 0) {
        std::fprintf(stderr, "impossible: digest is zero\n");
        std::exit(1);
    }
    digest_out = d;
    return static_cast<double>(w.ops.size()) * 1e9 / static_cast<double>(t1 - t0);
}

// Median, slowest and fastest of a set of rounds. The spread is reported
// because on this machine it is large and hiding it would be the whole problem.
void summarise(Result& r, std::vector<double> rounds) {
    r.ops_per_sec = median_of(rounds);
    std::sort(rounds.begin(), rounds.end());
    r.ops_lo = rounds.front();
    r.ops_hi = rounds.back();
    r.ns_per_op = 1e9 / r.ops_per_sec;
}

// Open loop at a fixed rate. The book is rebuilt first so the measured portion
// runs against a warm, realistically populated structure rather than an empty
// one -- otherwise the first thousand operations measure an empty book.
template <class Book>
void latency(const Workload& w, double rate, Samples& out) {
    Book book;

    // Warm the structure on the first half, untimed.
    const std::size_t split = w.ops.size() / 2;
    for (std::size_t i = 0; i < split; ++i) {
        apply(book, w.ops[i]);
    }

    out.clear();
    Pacer pacer{rate};
    pacer.start();
    u64 i = 0;
    for (std::size_t k = split; k < w.ops.size(); ++k, ++i) {
        const u64 due = pacer.due(i);
        pacer.wait_for(due);
        apply(book, w.ops[k]);
        const u64 done = now_ns();
        out.add(done > due ? done - due : 0);
    }
    const u64 d = itch::book::book_digest(book);
    do_not_optimize(d);
}

// The identical loop with nothing in it. Whatever this costs is the harness,
// not the book.
void harness_floor(std::size_t n, double rate, Samples& out) {
    out.clear();
    Pacer pacer{rate};
    pacer.start();
    volatile u64 sink = 0;
    for (u64 i = 0; i < n; ++i) {
        const u64 due = pacer.due(i);
        pacer.wait_for(due);
        sink = sink + 1;
        const u64 done = now_ns();
        out.add(done > due ? done - due : 0);
    }
    do_not_optimize(sink);
}

template <class Book>
void measure_latency(Result& r, const Workload& w, double rate) {
    Samples s{w.ops.size()};
    latency<Book>(w, rate, s);
    r.p50 = s.percentile(0.50);
    r.p99 = s.percentile(0.99);
    r.p999 = s.percentile(0.999);
    r.p9999 = s.percentile(0.9999);
    r.max = s.max();
    r.mean = s.mean();
}

std::string floor_note(u32 value, u64 resolution) {
    if (value <= resolution) {
        return "<= floor";
    }
    return std::to_string(value);
}

}  // namespace

int main(int argc, char** argv) {
    print_power_state();

    std::string corpus;
    std::string symbol = "QQQ";
    std::string only;
    bool        want_counters = false;
    int         runs = 5;
    double      rate = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--symbol" && i + 1 < argc) {
            symbol = argv[++i];
        } else if (a == "--only" && i + 1 < argc) {
            only = argv[++i];
        } else if (a == "--counters") {
            want_counters = true;
        } else if (a == "--runs" && i + 1 < argc) {
            runs = std::atoi(argv[++i]);
        } else if (a == "--rate" && i + 1 < argc) {
            rate = std::atof(argv[++i]);
        } else {
            corpus = a;
        }
    }
    if (corpus.empty()) {
        std::fprintf(stderr,
                     "usage: bench_book [--symbol SYM] [--runs N] [--rate OPS] <corpus>\n");
        return 2;
    }

#if defined(__APPLE__)
    // Ask for a performance core. macOS has no way to pin a thread to a core,
    // and no equivalent of isolcpus or nohz_full, so this is as close as this
    // machine gets. It is why the tail numbers here come with a harness floor
    // row: the scheduler is in the measurement and cannot be removed.
    ::pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    const u64    resolution = timer_resolution_ns();
    const double clock_cost = timer_call_cost_ns();

    std::printf("machine          Apple M4, macOS, %s\n",
#if defined(NDEBUG)
                "release"
#else
                "NOT A RELEASE BUILD -- numbers are meaningless"
#endif
    );
    std::printf("timer resolution %llu ns (measured)\n",
                static_cast<unsigned long long>(resolution));
    std::printf("clock read cost  %.2f ns (measured)\n", clock_cost);

    std::printf("\nloading workload for %s from %s\n", symbol.c_str(), corpus.c_str());
    const Workload w = load_workload(corpus, symbol);
    if (w.ops.empty()) {
        std::fprintf(stderr, "error: no book operations for %s\n", symbol.c_str());
        return 1;
    }
    std::printf("symbol           %s (locate %u)\n", w.symbol.c_str(), w.locate);
    std::printf("operations       %s\n", commas(w.ops.size()).c_str());
    std::printf("  add %s  execute %s  cancel %s  delete %s  replace %s\n",
                commas(w.adds).c_str(), commas(w.executes).c_str(),
                commas(w.cancels).c_str(), commas(w.deletes).c_str(),
                commas(w.replaces).c_str());

    // --only: one implementation, alone in this process, no cross-contamination.
    // This is where the absolute throughput number comes from.
    if (!only.empty()) {
        Result r;
        std::vector<double> rounds;
        u64 junk = 0;
        PerfCounters  pc;
        PerfCounters* cp = (want_counters && pc.available()) ? &pc : nullptr;
        if (only == "flat") {
            r.name = "FlatBook";
            for (int i = 0; i < 2; ++i) do_not_optimize(one_pass<FlatBook>(w, junk));
            for (int i = 0; i < runs; ++i) rounds.push_back(one_pass<FlatBook>(w, r.digest, cp));
        } else if (only == "avl") {
            r.name = "AvlBook";
            for (int i = 0; i < 2; ++i) do_not_optimize(one_pass<AvlBook>(w, junk));
            for (int i = 0; i < runs; ++i) rounds.push_back(one_pass<AvlBook>(w, r.digest, cp));
        } else if (only == "map") {
            r.name = "MapBook";
            for (int i = 0; i < 2; ++i) do_not_optimize(one_pass<MapBook>(w, junk));
            for (int i = 0; i < runs; ++i) rounds.push_back(one_pass<MapBook>(w, r.digest, cp));
        } else {
            std::fprintf(stderr, "unknown implementation '%s'\n", only.c_str());
            return 2;
        }
        summarise(r, std::move(rounds));
        std::printf("\nisolated: %s alone in this process, %d rounds\n\n", r.name.c_str(),
                    runs);
        std::printf("%-12s %14s %10s %14s %14s\n", "", "median ops/s", "ns/op", "slowest",
                    "fastest");
        std::printf("%s\n", std::string(68, '-').c_str());
        std::printf("%-12s %14s %10.1f %14s %14s\n", r.name.c_str(),
                    commas(static_cast<u64>(r.ops_per_sec)).c_str(), r.ns_per_op,
                    commas(static_cast<u64>(r.ops_lo)).c_str(),
                    commas(static_cast<u64>(r.ops_hi)).c_str());
        std::printf("digest       %016llx\n",
                    static_cast<unsigned long long>(r.digest));

        if (want_counters) {
            const CounterValues c = pc.totals();
            if (!c.ok) {
                std::printf("\ncounters     unavailable: %s\n", c.why_not.c_str());
            } else {
                // Per book operation, over exactly the timed loops and nothing
                // else. These are what explain a ratio between implementations.
                const double ops = static_cast<double>(w.ops.size()) * runs;
                const auto per = [&](u64 v) { return static_cast<double>(v) / ops; };
                std::printf("\ncounters, timed region only, per book operation (%d rounds)\n",
                            runs);
                std::printf("  cycles/op             %10.2f\n", per(c.cycles));
                std::printf("  instructions/op       %10.2f\n", per(c.instructions));
                std::printf("  IPC                   %10.3f\n",
                            c.cycles ? static_cast<double>(c.instructions) /
                                           static_cast<double>(c.cycles)
                                     : 0.0);
                std::printf("  branch-misses/op      %10.4f\n", per(c.branch_misses));
                std::printf("  LLC-misses/op         %10.4f\n", per(c.llc_misses));
                std::printf("  L1d-load-misses/op    %10.4f\n", per(c.l1d_misses));
                std::printf("  dTLB-load-misses/op   %10.4f\n", per(c.dtlb_misses));
                if (c.worst_running_fraction < 1.0) {
                    std::printf("  multiplexed: worst event ran %.1f%% of the time, scaled\n",
                                100.0 * c.worst_running_fraction);
                }
            }
        }
        return 0;
    }

    // Warmup, discarded. Two full passes each, so caches, branch predictors and
    // the CPU's frequency governor have all settled before anything is timed.
    for (int i = 0; i < 2; ++i) {
        u64 junk = 0;
        do_not_optimize(one_pass<FlatBook>(w, junk));
        do_not_optimize(one_pass<AvlBook>(w, junk));
        do_not_optimize(one_pass<MapBook>(w, junk));
    }

    // Rounds are INTERLEAVED, not grouped by implementation.
    //
    // This machine's throughput drifts over the first few seconds of a process:
    // measuring all of FlatBook's rounds first and all of MapBook's last gave
    // FlatBook the cold period every time, which is a systematic bias and not
    // noise. Round-robin gives all three the same conditions.
    std::vector<Result> results(3);
    results[0].name = "FlatBook";
    results[1].name = "AvlBook";
    results[2].name = "MapBook";
    std::vector<double> flat_rounds, avl_rounds, map_rounds;
    for (int round = 0; round < runs; ++round) {
        flat_rounds.push_back(one_pass<FlatBook>(w, results[0].digest));
        avl_rounds.push_back(one_pass<AvlBook>(w, results[1].digest));
        map_rounds.push_back(one_pass<MapBook>(w, results[2].digest));
    }
    summarise(results[0], std::move(flat_rounds));
    summarise(results[1], std::move(avl_rounds));
    summarise(results[2], std::move(map_rounds));

    double slowest = results[0].ops_per_sec;
    for (const Result& r : results) {
        slowest = std::min(slowest, r.ops_per_sec);
    }
    if (rate <= 0) {
        rate = 0.5 * slowest;
    }

    // All three must have produced the same book. A fast wrong answer is not a
    // result.
    for (std::size_t i = 1; i < results.size(); ++i) {
        if (results[i].digest != results[0].digest) {
            std::fprintf(stderr,
                         "error: %s produced a different book from %s (%llx vs %llx)\n",
                         results[i].name.c_str(), results[0].name.c_str(),
                         static_cast<unsigned long long>(results[i].digest),
                         static_cast<unsigned long long>(results[0].digest));
            return 1;
        }
    }
    std::printf("book digest      %016llx (all three agree)\n",
                static_cast<unsigned long long>(results[0].digest));

    measure_latency<FlatBook>(results[0], w, rate);
    measure_latency<AvlBook>(results[1], w, rate);
    measure_latency<MapBook>(results[2], w, rate);

    Samples floor_s{w.ops.size() / 2};
    harness_floor(w.ops.size() / 2, rate, floor_s);

    std::printf("\nthroughput, closed loop, %d INTERLEAVED rounds, one process\n", runs);
    std::printf("equal conditions for the comparison, but every absolute here is\n");
    std::printf("depressed by the other two implementations sharing the caches.\n");
    std::printf("run with --only for an isolated absolute.\n\n");
    std::printf("%-12s %14s %10s %14s %14s %8s\n", "", "median ops/s", "ns/op",
                "slowest", "fastest", "vs Flat");
    std::printf("%s\n", std::string(78, '-').c_str());
    for (const Result& r : results) {
        std::printf("%-12s %14s %10.1f %14s %14s %7.2fx\n", r.name.c_str(),
                    commas(static_cast<u64>(r.ops_per_sec)).c_str(), r.ns_per_op,
                    commas(static_cast<u64>(r.ops_lo)).c_str(),
                    commas(static_cast<u64>(r.ops_hi)).c_str(),
                    results[0].ops_per_sec / r.ops_per_sec);
    }

    std::printf("\nlatency per operation, open loop at %s ops/s, nanoseconds\n",
                commas(static_cast<u64>(rate)).c_str());
    std::printf("rate is 50%% of the slowest implementation's saturation, so none is backed up\n");
    std::printf("timer floor is %llu ns; a value at or below it is not a measurement\n\n",
                static_cast<unsigned long long>(resolution));
    std::printf("%-12s %10s %10s %10s %10s %10s %10s\n", "", "p50", "p99", "p99.9",
                "p99.99", "max", "mean");
    std::printf("%s\n", std::string(80, '-').c_str());
    std::printf("%-12s %10s %10s %10s %10s %10llu %10.1f\n", "harness floor",
                floor_note(floor_s.percentile(0.50), resolution).c_str(),
                floor_note(floor_s.percentile(0.99), resolution).c_str(),
                floor_note(floor_s.percentile(0.999), resolution).c_str(),
                floor_note(floor_s.percentile(0.9999), resolution).c_str(),
                static_cast<unsigned long long>(floor_s.max()), floor_s.mean());
    for (const Result& r : results) {
        std::printf("%-12s %10s %10s %10s %10s %10llu %10.1f\n", r.name.c_str(),
                    floor_note(r.p50, resolution).c_str(),
                    floor_note(r.p99, resolution).c_str(),
                    floor_note(r.p999, resolution).c_str(),
                    floor_note(r.p9999, resolution).c_str(),
                    static_cast<unsigned long long>(r.max), r.mean);
    }
    std::printf("\nthe harness floor row is the same loop with no book operation in it.\n");
    return 0;
}
