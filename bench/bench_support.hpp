// Timing, percentiles and rate pacing.
//
// Two things here are not boilerplate.
//
// First, the timer's resolution is measured, not assumed. On this machine
// hw.tbfrequency is 24 MHz, so every clock quantises to about 41.67 ns, and a
// book update is expected in the tens of nanoseconds. That is a hard limit on
// what can honestly be reported: the tail is measurable because a tail sample
// is large relative to the quantum, and the median frequently is not.
// timer_resolution_ns() measures it so the limit is a number in NUMBERS.md
// rather than a claim.
//
// Second, latency is measured open loop. A tight loop measures how fast the
// machine can go when nothing is waiting, which hides queueing entirely --
// coordinated omission. Samples are timed against a schedule fixed in advance,
// so an operation that runs late is recorded as late.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <cstdio>
#include <cstring>
#include <time.h>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace itch::bench {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Monotonic nanoseconds. CLOCK_UPTIME_RAW does not step and is not adjusted.
[[nodiscard]] inline u64 now_ns() noexcept {
#if defined(__APPLE__)
    return ::clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<u64>(ts.tv_sec) * 1'000'000'000ULL + static_cast<u64>(ts.tv_nsec);
#endif
}

// Smallest non-zero difference between consecutive reads, over many samples.
// This is the quantisation floor of every latency number in this project.
[[nodiscard]] inline u64 timer_resolution_ns(int samples = 200000) {
    u64 best = ~u64{0};
    u64 prev = now_ns();
    for (int i = 0; i < samples; ++i) {
        const u64 t = now_ns();
        const u64 d = t - prev;
        if (d != 0 && d < best) {
            best = d;
        }
        prev = t;
    }
    return best == ~u64{0} ? 0 : best;
}

// Cost of reading the clock, which is subtracted from nothing but is reported
// so that a latency near the timer floor can be read for what it is.
[[nodiscard]] inline double timer_call_cost_ns(int samples = 200000) {
    const u64 t0 = now_ns();
    for (int i = 0; i < samples; ++i) {
        const u64 t = now_ns();
        asm volatile("" : : "r"(t) : "memory");
    }
    const u64 t1 = now_ns();
    return static_cast<double>(t1 - t0) / samples;
}

template <class T>
inline void do_not_optimize(const T& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

inline void clobber_memory() { asm volatile("" : : : "memory"); }

// Hardware counters scoped to exactly the timed region. Linux only.
//
// Why not `perf stat` around the process: bench_book builds its workload by
// inflating the 4.7 GB corpus and dispatching 368 million messages, about thirty
// seconds, and then times the book for a fraction of a second. Process-wide
// counters would be almost entirely gzip and would still look like they
// explained the ratios between book implementations. These are opened by the
// benchmark itself and enabled only around the loop being measured.
//
// Events are opened independently rather than as one group. A laptop PMU has
// few general-purpose counters and a group is scheduled all or nothing; single
// events are multiplexed instead, and the enabled/running times are read back so
// the scaling can be applied and reported rather than hidden.
//
// On hybrid Intel parts the generic hardware events must name the P-core PMU in
// the top 32 bits of config, or they may count nothing on the core the thread is
// pinned to. That type is read from sysfs when it exists.
struct CounterValues {
    bool        ok = false;
    std::string why_not;
    u64         cycles = 0;
    u64         instructions = 0;
    u64         branch_misses = 0;
    u64         llc_misses = 0;
    u64         l1d_misses = 0;
    u64         dtlb_misses = 0;
    double      worst_running_fraction = 1.0;  // < 1 means multiplexed and scaled
};

#if defined(__linux__)
class PerfCounters {
public:
    PerfCounters() {
        const u64 hybrid = hybrid_core_type();
        const u64 cache_l1d_read_miss = PERF_COUNT_HW_CACHE_L1D |
                                        (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                                        (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
        const u64 cache_dtlb_read_miss = PERF_COUNT_HW_CACHE_DTLB |
                                         (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                                         (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
        add(PERF_TYPE_HARDWARE, hybrid | PERF_COUNT_HW_CPU_CYCLES, &CounterValues::cycles);
        add(PERF_TYPE_HARDWARE, hybrid | PERF_COUNT_HW_INSTRUCTIONS,
            &CounterValues::instructions);
        add(PERF_TYPE_HARDWARE, hybrid | PERF_COUNT_HW_BRANCH_MISSES,
            &CounterValues::branch_misses);
        add(PERF_TYPE_HARDWARE, hybrid | PERF_COUNT_HW_CACHE_MISSES,
            &CounterValues::llc_misses);
        add(PERF_TYPE_HW_CACHE, hybrid | cache_l1d_read_miss, &CounterValues::l1d_misses);
        add(PERF_TYPE_HW_CACHE, hybrid | cache_dtlb_read_miss, &CounterValues::dtlb_misses);
    }

    ~PerfCounters() {
        for (Ev& e : evs_) {
            if (e.fd >= 0) {
                ::close(e.fd);
            }
        }
    }

    PerfCounters(const PerfCounters&) = delete;
    PerfCounters& operator=(const PerfCounters&) = delete;

    [[nodiscard]] bool available() const noexcept { return opened_ > 0; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    void start() {
        for (Ev& e : evs_) {
            if (e.fd >= 0) {
                ::ioctl(e.fd, PERF_EVENT_IOC_RESET, 0);
                ::ioctl(e.fd, PERF_EVENT_IOC_ENABLE, 0);
            }
        }
    }

    // Stops and adds this region's scaled counts to the running totals.
    void stop() {
        for (Ev& e : evs_) {
            if (e.fd >= 0) {
                ::ioctl(e.fd, PERF_EVENT_IOC_DISABLE, 0);
            }
        }
        for (Ev& e : evs_) {
            if (e.fd < 0) {
                continue;
            }
            u64 buf[3] = {0, 0, 0};  // value, time_enabled, time_running
            if (::read(e.fd, buf, sizeof(buf)) != static_cast<ssize_t>(sizeof(buf))) {
                continue;
            }
            double scaled = static_cast<double>(buf[0]);
            if (buf[2] != 0 && buf[2] < buf[1]) {
                const double frac = static_cast<double>(buf[2]) / static_cast<double>(buf[1]);
                scaled /= frac;
                worst_ = frac < worst_ ? frac : worst_;
            }
            totals_.*(e.slot) += static_cast<u64>(scaled);
        }
    }

    [[nodiscard]] CounterValues totals() const {
        CounterValues v = totals_;
        v.ok = available();
        v.why_not = error_;
        v.worst_running_fraction = worst_;
        return v;
    }

private:
    struct Ev {
        int                     fd = -1;
        u64 CounterValues::*    slot = nullptr;
    };

    static u64 hybrid_core_type() {
        std::FILE* f = std::fopen("/sys/bus/event_source/devices/cpu_core/type", "r");
        if (f == nullptr) {
            return 0;
        }
        unsigned long long t = 0;
        const int n = std::fscanf(f, "%llu", &t);
        std::fclose(f);
        return n == 1 ? (static_cast<u64>(t) << 32) : 0;
    }

    void add(u32 type, u64 config, u64 CounterValues::*slot) {
        struct perf_event_attr attr;
        std::memset(&attr, 0, sizeof(attr));
        attr.size = sizeof(attr);
        attr.type = type;
        attr.config = config;
        attr.disabled = 1;
        attr.exclude_kernel = 1;  // user space only: works at perf_event_paranoid <= 2
        attr.exclude_hv = 1;
        attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
        const long fd = ::syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
        Ev e;
        e.slot = slot;
        if (fd < 0) {
            if (error_.empty()) {
                error_ = std::string("perf_event_open: ") + std::strerror(errno);
            }
        } else {
            e.fd = static_cast<int>(fd);
            ++opened_;
        }
        evs_.push_back(e);
    }

    std::vector<Ev> evs_;
    CounterValues   totals_{};
    std::string     error_;
    int             opened_ = 0;
    double          worst_ = 1.0;
};
#else
class PerfCounters {
public:
    [[nodiscard]] bool available() const noexcept { return false; }
    [[nodiscard]] std::string error() const { return "hardware counters are Linux only"; }
    void start() {}
    void stop() {}
    [[nodiscard]] CounterValues totals() const {
        CounterValues v;
        v.why_not = error();
        return v;
    }
};
#endif

// Deterministic generator for synthetic benchmark flows. Same splitmix64 as the
// test harness, so a benchmark's workload reproduces exactly from its seed.
class Rng {
public:
    explicit constexpr Rng(u64 seed) noexcept : s_(seed) {}

    constexpr u64 next() noexcept {
        u64 z = (s_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    constexpr u64 range(u64 lo, u64 hi) noexcept {
        const u64 span = hi - lo + 1;
        return lo + static_cast<u64>((static_cast<__uint128_t>(next()) * span) >> 64);
    }

    constexpr bool chance(u32 percent) noexcept { return range(0, 99) < percent; }

private:
    u64 s_;
};

// Exact percentiles: every sample is kept and sorted, so there is no bucketing
// error anywhere in the distribution. At 10 million samples that is 40 MB, which
// is a fair price for not having to caveat the tail.
class Samples {
public:
    explicit Samples(std::size_t reserve = 0) {
        if (reserve != 0) {
            v_.reserve(reserve);
        }
    }

    void add(u64 ns) { v_.push_back(static_cast<u32>(std::min<u64>(ns, ~u32{0}))); }
    void clear() {
        v_.clear();
        sorted_ = false;
    }

    [[nodiscard]] std::size_t count() const noexcept { return v_.size(); }
    [[nodiscard]] bool empty() const noexcept { return v_.empty(); }

    [[nodiscard]] u32 percentile(double p) {
        sort();
        if (v_.empty()) {
            return 0;
        }
        const auto i = static_cast<std::size_t>(p * static_cast<double>(v_.size() - 1));
        return v_[std::min(i, v_.size() - 1)];
    }

    [[nodiscard]] u32 min() { return percentile(0.0); }
    [[nodiscard]] u32 max() { return percentile(1.0); }

    [[nodiscard]] double mean() const {
        if (v_.empty()) {
            return 0.0;
        }
        u64 sum = 0;
        for (u32 x : v_) {
            sum += x;
        }
        return static_cast<double>(sum) / static_cast<double>(v_.size());
    }

private:
    void sort() {
        if (!sorted_) {
            std::sort(v_.begin(), v_.end());
            sorted_ = true;
        }
    }

    std::vector<u32> v_;
    bool             sorted_ = false;
};

// Open-loop injection. Operation i is due at start + i * interval, whatever
// happened to operations 0..i-1. An operation that runs late is recorded late,
// which is the point.
class Pacer {
public:
    explicit Pacer(double ops_per_second)
        : interval_ns_(ops_per_second > 0
                           ? static_cast<u64>(1e9 / ops_per_second)
                           : 0) {}

    void start() noexcept { start_ = now_ns(); }

    [[nodiscard]] u64 due(u64 i) const noexcept { return start_ + i * interval_ns_; }

    // Spins rather than sleeping: at these intervals any sleep would overshoot
    // by more than the thing being measured.
    //
    // The hint instruction is architecture specific. `isb` is arm64 only and
    // will not assemble on x86-64, which matters because the tail latency work
    // this harness exists for is Linux box work -- CI on ubuntu is what caught
    // it, having never been run before.
    void wait_for(u64 deadline) const noexcept {
        while (now_ns() < deadline) {
            spin_hint();
        }
    }

    static void spin_hint() noexcept {
#if defined(__aarch64__)
        asm volatile("isb" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
        asm volatile("pause" ::: "memory");
#else
        asm volatile("" ::: "memory");
#endif
    }

    [[nodiscard]] u64 interval_ns() const noexcept { return interval_ns_; }

private:
    u64 interval_ns_ = 0;
    u64 start_ = 0;
};

// Median of several independent runs, which is what gets reported. A single run
// is a sample of the machine's mood, not a measurement.
[[nodiscard]] inline double median_of(std::vector<double> runs) {
    std::sort(runs.begin(), runs.end());
    const std::size_t n = runs.size();
    if (n == 0) {
        return 0.0;
    }
    return (n % 2 == 1) ? runs[n / 2] : 0.5 * (runs[n / 2 - 1] + runs[n / 2]);
}

// The machine's power state, printed above every result.
//
// This is not decoration. The same binary on the same input measured 65.7M
// ops/s with Low Power Mode off and 33.5M with it on -- a 49% drop hitting all
// three book implementations almost equally, while their ratios held.
//
// It is Low Power Mode specifically, not the power source. That was measured
// too: battery with the mode off is slightly faster than the run originally
// recorded as "mains", so the source is not the variable and this used to warn
// about the wrong thing.
struct PowerState {
    std::string source = "unknown";
    bool        low_power_mode = false;
    bool        known = false;
};

[[nodiscard]] inline std::string run_capture(const char* cmd) {
    std::string out;
    std::FILE*  f = ::popen(cmd, "r");
    if (f == nullptr) {
        return out;
    }
    char buf[256];
    while (std::fgets(buf, sizeof(buf), f) != nullptr) {
        out += buf;
    }
    ::pclose(f);
    return out;
}

[[nodiscard]] inline PowerState power_state() {
    PowerState ps;
#if defined(__APPLE__)
    const std::string batt = run_capture("pmset -g batt 2>/dev/null");
    if (batt.find("'AC Power'") != std::string::npos) {
        ps.source = "AC";
        ps.known = true;
    } else if (batt.find("'Battery Power'") != std::string::npos) {
        ps.source = "battery";
        ps.known = true;
    }
    const std::string mode = run_capture("pmset -g 2>/dev/null | grep lowpowermode");
    ps.low_power_mode = mode.find(" 1") != std::string::npos;
#endif
    return ps;
}

// Prints it, and says plainly when the state will depress the numbers.
inline void print_power_state() {
    const PowerState ps = power_state();
    if (!ps.known) {
        std::printf("power state      unknown\n");
        return;
    }
    std::printf("power state      %s, low power mode %s\n", ps.source.c_str(),
                ps.low_power_mode ? "ON" : "off");
    if (ps.low_power_mode) {
        std::printf("                 ^ Low Power Mode roughly halves absolute throughput.\n");
        std::printf("                   These numbers are NOT comparable to a run with it\n");
        std::printf("                   off. The ratios between implementations are.\n");
    }
}

inline std::string commas(u64 v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(static_cast<std::size_t>(i), ",");
    }
    return s;
}

}  // namespace itch::bench
