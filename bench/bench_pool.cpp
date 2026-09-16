// Order pool against the general-purpose allocator, and the prefault check.
//
// Two things worth stating about the comparison. It is deliberately a working
// set the size of a real book, not a handful of objects: malloc's thread cache
// makes a tiny allocate/free loop look excellent and says nothing about what
// happens with thousands of live objects and a churn pattern that outlives the
// cache. And the pool is measured with its generation checking ON, because that
// is how it ships -- turning it off for the benchmark would be measuring
// something nobody runs.

#include <cstdio>
#include <memory>
#include <vector>

#include <sys/resource.h>

#include "bench_support.hpp"
#include "itch/book/pool.hpp"
#include "itch/book/records.hpp"

using namespace itch;
using namespace itch::bench;
using namespace itch::book;

namespace {

using OrderPool = Pool<Order, OrderTag>;

// Live orders held at once. Chosen from the measured peaks: QQQ 7,679,
// AMD 15,286, AAPL 42,774.
constexpr int kCycles = 4'000'000;

// Two working sets. malloc's thread cache makes a small one look excellent, so
// a single size is not evidence; 8k is around the measured peak for QQQ and
// 64k is past AAPL's 42,774.
std::size_t g_live = 8192;
#define kLive g_live

// A churn pattern with a realistic shape: keep kLive objects alive, and each
// step free a pseudo-random one and allocate a replacement.
double bench_pool(u64 seed) {
    OrderPool pool{static_cast<u32>(kLive) * 2, /*quarantine=*/0};
    std::vector<OrderHandle> live;
    live.reserve(kLive);
    for (std::size_t i = 0; i < kLive; ++i) {
        live.push_back(pool.allocate());
    }
    itch::bench::u64 acc = 0;
    std::uint64_t    s = seed;
    const u64        t0 = now_ns();
    for (int i = 0; i < kCycles; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::size_t k = (s >> 33) % kLive;
        pool.free(live[k]);
        live[k] = pool.allocate();
        pool[live[k]].ref = static_cast<OrderRef>(i);
        acc += pool[live[k]].ref;
    }
    const u64 t1 = now_ns();
    do_not_optimize(acc);
    return static_cast<double>(t1 - t0) / kCycles;
}

double bench_new_delete(u64 seed) {
    std::vector<Order*> live;
    live.reserve(kLive);
    for (std::size_t i = 0; i < kLive; ++i) {
        live.push_back(new Order{});
    }
    itch::bench::u64 acc = 0;
    std::uint64_t    s = seed;
    const u64        t0 = now_ns();
    for (int i = 0; i < kCycles; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::size_t k = (s >> 33) % kLive;
        delete live[k];
        live[k] = new Order{};
        live[k]->ref = static_cast<OrderRef>(i);
        acc += live[k]->ref;
    }
    const u64 t1 = now_ns();
    do_not_optimize(acc);
    for (Order* p : live) {
        delete p;
    }
    return static_cast<double>(t1 - t0) / kCycles;
}

long minor_faults() {
    struct rusage ru {};
    ::getrusage(RUSAGE_SELF, &ru);
    return ru.ru_minflt;
}

}  // namespace

int main(int argc, char** argv) {
    int runs = 5;
    if (argc > 2 && std::string(argv[1]) == "--runs") {
        runs = std::atoi(argv[2]);
    }

    std::printf("cycles per round   %s\n", commas(kCycles).c_str());
    std::printf("sizeof(Order)      %zu bytes\n\n", sizeof(Order));
    std::printf("median of %d interleaved rounds, one free + one allocate per cycle\n\n",
                runs);
    std::printf("%-14s %16s %16s %10s\n", "live objects", "pool ns/cycle",
                "new+delete ns/cycle", "pool wins");
    std::printf("%s\n", std::string(62, '-').c_str());

    for (std::size_t live : {std::size_t{8192}, std::size_t{65536}}) {
        g_live = live;
        do_not_optimize(bench_pool(1));
        do_not_optimize(bench_new_delete(1));
        std::vector<double> pool_ns, heap_ns;
        for (int r = 0; r < runs; ++r) {  // interleaved
            pool_ns.push_back(bench_pool(static_cast<u64>(r) + 7));
            heap_ns.push_back(bench_new_delete(static_cast<u64>(r) + 7));
        }
        const double p = median_of(pool_ns);
        const double h = median_of(heap_ns);
        std::printf("%-14s %16.2f %16.2f %9.2fx\n", commas(live).c_str(), p, h, h / p);
    }

    // Prefault: after construction, a full pass over every slot must take no
    // minor faults. That is the claim prefaulting makes, so it gets checked
    // rather than asserted.
    //
    // The handle vector is reserved AND touched before the measurement starts.
    // Measuring it inside the window attributes this harness's own first-touch
    // faults to the pool, which is what the first version of this did.
    std::printf("\nprefault check\n");
    constexpr u32 kSlots = 1u << 18;
    std::vector<OrderHandle> hs(kSlots, OrderHandle{});
    do_not_optimize(hs.data());

    const long before_ctor = minor_faults();
    OrderPool  fresh{kSlots, 0};
    const long after_ctor = minor_faults();

    itch::bench::u64 acc = 0;
    for (u32 i = 0; i < kSlots; ++i) {
        hs[i] = fresh.allocate();
    }
    for (const OrderHandle hh : hs) {
        fresh[hh].ref = 1;
        acc += fresh[hh].ref;
    }
    const long after_touch = minor_faults();
    do_not_optimize(acc);
    std::printf("  minor faults during construction (prefault)  %ld\n",
                after_ctor - before_ctor);
    std::printf("  minor faults touching all %s slots after     %ld\n",
                commas(kSlots).c_str(), after_touch - after_ctor);
    std::printf("\nthe second number is the one that matters: zero means the hot path\n");
    std::printf("never takes a first-touch fault.\n");
    return 0;
}
