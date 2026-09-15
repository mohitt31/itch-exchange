# Numbers

Every number in the README appears here with the command that produced it, the
machine it ran on, and the build flags. Nothing in this file is an estimate.
Where something has not been measured yet it says so.

`bench/reproduce.sh` regenerates every number here in one command.

---

## Machines

### M4 (primary)

| | |
|---|---|
| CPU | Apple M4, 4 performance + 6 efficiency cores |
| OS | macOS 26.5 (Darwin 25.5.0, arm64) |
| Compiler | Apple clang 21.0.0 (clang-2100.1.1.101) |
| RAM | 16 GiB |
| Cache line | 128 B |
| Page size | 16 KiB |
| L1d | 128 KiB (P-core), 64 KiB (E-core) |
| L2 | 16 MiB (P-core), 4 MiB (E-core) |

```
sysctl -n machdep.cpu.brand_string hw.ncpu hw.perflevel0.physicalcpu hw.perflevel1.physicalcpu \
             hw.cachelinesize hw.pagesize hw.perflevel0.l1dcachesize hw.perflevel0.l2cachesize \
             hw.l1dcachesize hw.l2cachesize hw.memsize
```

### Linux box (secondary)

Not yet used. Fine-grained p50 latency and PMU counters are scheduled here; see
"Not measured" below.

---

## Timer resolution on M4 — measured, and it constrains what can be reported

```
sysctl -n hw.tbfrequency
24000000
```

The system timebase runs at 24 MHz, so `mach_absolute_time` and
`clock_gettime(CLOCK_MONOTONIC_RAW)` both quantise to **41.67 ns**.

A single book update is expected to be in the tens of nanoseconds. That has a
direct consequence for what this project is allowed to claim:

- **Throughput** on M4 is accurate. It is measured over millions of operations,
  so the timer quantum is a rounding error.
- **p99 and p99.9** on M4 are meaningful. Tail samples are large enough that a
  41.67 ns quantum is a small relative error.
- **p50 per operation** on M4 is *not* honestly measurable. If it lands in the
  first bucket it will be reported as "below timer resolution (<= 42 ns)" and
  never as a specific number.

The fine-grained p50 comes from the Linux box via `rdtsc` and will be labelled
with that machine.

## Performance counters on M4 — not available

```
xcrun xctrace version   -> fails (Xcode not installed, only Command Line Tools)
which perf              -> not found
```

There is no `perf` on macOS and no Instruments without full Xcode. Cache-miss
and branch-miss counters, and therefore the counter-based explanations of the
three-way benchmark ratios, are Linux-box work. Profiling on M4 is stack
sampling via `/usr/bin/sample`.

---

## Input data

Source: `https://emi.nasdaq.com/ITCH/Nasdaq ITCH/`

| File | Size (gzip) | Status |
|---|---|---|
| `01302019.NASDAQ_ITCH50.gz` | 4,764,426,091 B | intended corpus |
| `01302020.NASDAQ_ITCH50.gz` | 5,597,158,940 B | alternative |
| `01302018.NASDAQ_ITCH50.gz` | 1,245 B | broken stub, unusable |

Specification: `NQTVITCHspecification.pdf`, 1,200,722 B,
sha256 `45e0531d1b4b3beb886e9618b2ab824a5aa9bda3a99c0dff03509306e68aacc3`.
It defines 23 message types (the April 2023 revision added `'O'`).

```
curl -sI "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz"
```

Free disk at project start: 4,654,206,976 B (`diskutil info /System/Volumes/Data`).
The compressed file alone does not fit, so the corpus is built as a bounded byte
range of the gzip stream, filtered to a single symbol. `tools/fetch_data.sh`
records the exact URL, byte range, ETag and sha256 of whatever it fetched, and
that record lands in this file. It does not exist yet.

---

## Build configurations

| Preset | Flags | Invariant level |
|---|---|---|
| `release` | `-O3 -DNDEBUG -mcpu=native` | 0 |
| `release-checked` | `-O3 -DNDEBUG -mcpu=native` | 1 |
| `asan-ubsan` | `-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all` | 2 |
| `tsan` | `-O1 -g -fno-omit-frame-pointer -fsanitize=thread` | 2 |

All configurations additionally use `-std=c++20 -Wall -Wextra -Wpedantic -Werror
-Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor
-Wformat=2 -Wdouble-promotion`.

`-mcpu=native` resolves to `-mcpu=apple-m4` on this machine. It is disabled in
the sanitizer presets so that those builds are portable to CI runners.

---

## Test results

19 tests, passing under all four configurations.

```
for p in release release-checked asan-ubsan tsan; do
  cmake --preset $p && cmake --build --preset $p && ctest --preset $p
done
```

| Preset | Result |
|---|---|
| `release` | 1/1 suites, 19 tests, 0 failed |
| `release-checked` | 1/1 suites, 19 tests, 0 failed |
| `asan-ubsan` | 1/1 suites, 19 tests, 0 failed |
| `tsan` | 1/1 suites, 19 tests, 0 failed |

After slice 6 (pools), 91 tests across 7 suites, passing under all four
configurations, plus 451 `static_assert`s on message sizes and field offsets.

| Suite | Tests |
|---|---|
| `test_core` | 19 |
| `test_wire` | 9 |
| `test_framing` | 14 |
| `test_parser` | 11 |
| `test_gzip` | 6 |
| `test_map_book` | 15 |
| `test_pool` | 17 |
| `test_ladder` | 12 |
| `test_flat_book` | 9 |
| `test_order_index` | 10 |

Total 122 after slice 8 (flat book and order index).

The three-way differential suite was mutation-tested: three deliberate bugs
injected one at a time into `FlatBook`, each caught. Queue tail not updated ->
an invariant assertion in `add`. Best not recomputed when the top level clears
-> the differential comparison. Cleared level left in the ladder -> the pool's
stale-handle detector.

The `tsan` result is **trivially clean**: the pipeline is single-threaded by
design and the cut list forbids threading inside the book. The configuration is
in CI as hygiene, not as evidence that any concurrency was verified.

---

## Real feed, complete session

Input: `01302019.NASDAQ_ITCH50.gz`, the complete 30 January 2019 NASDAQ
TotalView-ITCH 5.0 session. 4,764,426,091 bytes compressed, `gzip -t` clean.
Metadata including the sha256 is in `data/CORPUS.json`.

```
./build/release/apps/itch_stats --by-symbol 25 data/01302019.NASDAQ_ITCH50.gz
```

| | |
|---|---|
| messages | **368,366,634** |
| payload bytes | 10,509,149,824 |
| framed bytes | 11,245,883,092 |
| session | 03:03:59.687 to 20:05:00.000 Eastern |
| **malformed frames** | **0** |
| terminated | clean end of input |

Zero malformed frames means the framing layer's check -- each length prefix
against the length declared for its type byte -- passed **368,366,634 times**.
That is every one of the 23 layout structs validated against the real feed.

Message mix: `A` 44.24%, `D` 42.97%, `U` 7.39%, `E` 2.20%, `X` 1.27%, `I` 1.00%,
`F` 0.47%, `P` 0.36%, `L` 0.053%, `C` 0.043%, remainder below 0.01%. Eighteen of
the 23 defined types appear; `K N O W h` do not occur in this session.

Busiest symbols by book-moving messages: QQQ 3,448,973; SPY 3,055,956;
IWM 1,968,160; AMD 1,862,837; GOOG 1,671,883; AAPL 1,656,597.

The 14,356,335 msg/s reported by that run includes gzip inflate and is **not** a
parser benchmark.

## Price ladder measurements

```
./build/release/apps/itch_histogram --symbol QQQ --json measurements/histogram_QQQ.json \
    data/01302019.NASDAQ_ITCH50.gz
```

Full output in `measurements/`. Distance from the same side's inside, in
pennies, measured before the order is applied. Complete session.

| | QQQ | SPY | AMD |
|---|---|---|---|
| adds measured | 1,812,938 | 1,637,792 | 970,510 |
| p50 ticks | 0 | 1 | 0 |
| p90 ticks | 9 | 19 | 24 |
| p99 ticks | 498 | 803 | 340 |
| p99.9 ticks | 2,541 | 1,209 | 1,332 |
| **coverage +/-2048** | **99.8368%** | **99.9309%** | **99.9694%** |
| coverage +/-4096 | 99.9604% | 99.9742% | 99.9840% |
| sub-penny adds | 4 (0.00022%) | 2 (0.00012%) | 5 (0.00052%) |
| peak live levels | 2,137 | 940 | 1,855 |
| peak live orders | 7,679 | 2,985 | **15,286** |
| price range | $0.0001 - $199,999.99 | same | same |
| **crossed states** | **0** | **0** | **0** |

These decide the ladder geometry; the reasoning is in DESIGN.md section 15.

The full session is **stronger** evidence than the 490 MB prefix these were
first measured on: the +/-2048 window covers 99.84% to 99.97% of insertions over
a whole day, against 98.94% to 99.86% over the prefix, and sub-penny prices are
rarer still. The peak live order count grew from 13,843 to 15,286 on AMD, which
is what sizes the order index.

## Three-way book benchmark

```
./build/release/bench/bench_book --symbol QQQ --runs 7 data/01302019.NASDAQ_ITCH50.gz
```

Machine: Apple M4, macOS 26.5, Apple clang 21.0.0.
Build: `release` preset, `-O3 -DNDEBUG -mcpu=native` (resolves to `-mcpu=apple-m4`),
`ITCH_INVARIANT_LEVEL=0`.
Workload: QQQ's **3,448,973** real book operations over the complete session
(1,623,619 adds, 1,577,106 deletes, 189,321 replaces, 55,963 executions,
2,964 cancels).
All three implementations produce book digest `a0ed37f623c3aa3f`; the benchmark
refuses to report if they disagree.

### Throughput -- reliable

Closed loop, seven rounds, **interleaved** between implementations. Four separate
process invocations gave FlatBook 30,732,330 / 30,519,116 / 30,817,216 /
30,820,830 ops/s -- about 1% spread.

| | median ops/s | ns/op | slowest round | fastest round | vs FlatBook |
|---|---|---|---|---|---|
| **FlatBook** | **30,732,330** | **32.5** | 30,324,063 | 30,894,187 | 1.00x |
| AvlBook | 15,130,586 | 66.1 | 15,021,435 | 15,166,262 | **2.03x** |
| MapBook | 8,175,191 | 122.3 | 8,058,407 | 8,309,904 | **3.76x** |

`AvlBook` differs from `FlatBook` in exactly one thing: the price-to-level
structure. Same pools, same intrusive queues, same order index, an AVL tree over
a node pool instead of the tick-indexed ladder. So **2.03x is what the ladder
bought**. `MapBook` is the textbook implementation and changes everything at
once, so **3.76x is what the whole design bought**.

### Latency p50 -- reliable, with its range

Open loop at 4,087,595 ops/s (50% of the slowest implementation's saturation, so
none is backed up). Measured timer floor is 41 ns and a clock read costs about
15 ns, both reported by the benchmark at startup.

Across four separate invocations:

| | observed p50 (ns) | typical |
|---|---|---|
| harness floor | at or below the 41 ns timer floor | -- |
| FlatBook | 67, 70, 68, 102 | **~70** |
| AvlBook | 103, 106, 104, 123 | **~105** |
| MapBook | 160, 155, 159, 154 | **~156** |

Well clear of the timer floor, and the ratios agree with the throughput ratios.

### Latency p99 and beyond -- NOT reliable on this machine, with proof

The benchmark runs the identical pacing loop with **no book operation in it** and
reports it as a "harness floor" row.

FlatBook's p99, same binary, same input, four consecutive invocations:

| run | FlatBook p99 |
|---|---|
| 1 | 202 ns |
| 2 | 37,085 ns |
| 3 | 168 ns |
| 4 | **5,500,354 ns** |

**A 30,000x range for the same code on the same data.** In other runs the
*empty* loop's own p99 was 454 ns against 70-85 ns elsewhere, and its p99.9
reached 13,535 ns -- above FlatBook's 9,421 ns in the same run.

macOS has no `isolcpus`, no `nohz_full`, and no way to pin a thread to a core.
The benchmark requests `QOS_CLASS_USER_INTERACTIVE`, which is as close as this
machine gets and is not close enough.

**So no p99 or p99.9 figure is quoted from this machine anywhere in this
project.** The tail belongs on a box with core isolation. Until that runs, the
number does not exist. The figures above are recorded as evidence for why, not
as results.

## Profiling: before and after

Two bottlenecks found and fixed. Reasoning in DESIGN.md section 20.

### Order index sizing

The index was sized from the order pool's capacity, giving a 524,288-entry,
8 MiB table holding under 7,000 live entries. A sweep with the sizes
**interleaved** so warm-up drift could not land on one of them:

| entries | load factor | ops/s |
|---|---|---|
| 16,384 | 43.2% | 18,836,147 |
| 32,768 | 21.6% | 23,466,717 |
| 65,536 | 10.8% | 25,875,752 |
| **131,072** | **5.4%** | **26,974,316** |
| 262,144 | 2.7% | 24,844,157 |
| 524,288 | 1.3% | 20,698,310 |

There is an optimum and both sides of it are worse. Default changed to 131,072
entries. At the busiest measured peak (15,286 live orders on AMD) that is an
11.7% load factor, inside the flat part of the curve.

### Handle validation

`Pool<Order>::deref_checked` was 15.6% of the profile after the first fix. It
did four checks; two were provably redundant (the null check is subsumed by the
bounds check, the liveness check by the generation match). Reduced to two, with
no loss of detection: all 17 pool tests, including null, out-of-range,
use-after-free, double-free and slot-reuse, unchanged and passing.

### Combined

| | before | after | |
|---|---|---|---|
| FlatBook, 475k-op workload | 20,010,189 ops/s | 28,803,939 ops/s | 1.44x |
| index sizing alone | 20,010,189 | 26,492,637 | 1.32x |
| handle validation alone | 26,811,927-27,306,964 | 29,717,177-30,098,291 | 1.11x |

### One fix that did not pay, recorded because it did not

Slimming the assertion call sites to let `deref_checked` inline showed
26.5 -> 27.7 million ops/s on one run, a 4.5% win. Three further runs of each
version gave 26.8 / 27.3 / 27.1 against 27.2 to 27.7 -- overlapping -- and the
binary contained the same number of out-of-line `deref_checked` symbols either
way. It was noise, and the change was reverted rather than kept with a number
attached.

## Not measured yet

Listed so that their absence is explicit rather than quiet.

- All of the above over the complete session rather than a 490 MB prefix.
- Book update p99 and p99.9. Measured here but **scheduler-dominated and not
  quotable**; see above. **Linux box**, with `isolcpus` and `nohz_full`.
- Cache-miss and branch-miss counters explaining the 1.46x and 2.29x ratios.
  **Linux box.**
- The 24-byte order record variant, against the 32-byte one in use.
- `IndexHash::Identity` against `IndexHash::Mixed`. Both are built and tested;
  neither is benchmarked yet.
- Parse throughput on its own, without a decompressor in the loop.
- Matching engine throughput and latency.
- Before/after for the two bottlenecks found by profiling.
