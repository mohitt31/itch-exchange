# Numbers

Every number in the README appears here with the command that produced it, the
machine it ran on, and the build flags. Nothing in this file is an estimate.
Where something has not been measured yet it says so.

`bench/reproduce.sh` regenerates the whole file. It does not exist yet.

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

## Real feed, 490 MB prefix

Input: first 514,326,528 bytes of `01302019.NASDAQ_ITCH50.gz`,
sha256 `b837984685d826570e48704e0dcadd1dde03f310b4a4f6e9b16f0795e03998dc`.
A gzip prefix decompresses cleanly to the cut, so this is a valid partial
session covering 03:03:59 to 09:52:30 Eastern.

```
./build/release/apps/itch_stats --by-symbol 20 <prefix.gz>
```

| | |
|---|---|
| messages | 40,385,598 |
| payload bytes | 1,176,685,643 |
| framed bytes | 1,257,456,839 |
| malformed frames | **0** |
| terminated | truncated input, 36 trailing bytes (expected: it is a prefix) |

Zero malformed frames means the framing layer's check -- each length prefix
against the declared length for its type -- passed 40,385,598 times. That is
every one of the 23 layout structs validated against the real feed.

Message mix: `A` 42.25%, `D` 39.25%, `U` 7.67%, `X` 3.56%, `I` 2.60%,
`F` 2.19%, `E` 1.55%, `L` 0.48%, `P` 0.33%, `C` 0.036%, remainder below 0.03%.
Six types defined by the spec did not appear in this prefix: `B K W h N O`.

Throughput on this run was 14,501,907 msg/s and 430.6 MiB/s, but that figure
includes gzip inflate and is **not** a parser benchmark. It is reported here
only to show the run completed; the parser benchmark is still to be built.

Busiest symbols by book-moving messages: QQQ 475,247; RDS.B 369,811;
SPY 355,817; SAP 343,885; RDS.A 330,208; AMD 318,476.

## Price ladder measurements

```
./build/release/apps/itch_histogram --symbol QQQ --json measurements/QQQ_partial.json <prefix.gz>
```

Full output in `measurements/`. Distance from the same side's inside, in
pennies, measured before the order is applied.

| | QQQ | SPY | AMD |
|---|---|---|---|
| adds measured | 251,286 | 189,464 | 175,078 |
| p50 ticks | 0 | 1 | 0 |
| p90 ticks | 9 | 17 | 67 |
| p99 ticks | 2,138 | 797 | 1,068 |
| p99.9 ticks | 6,378 | 5,299 | 2,156 |
| coverage +/-2048 | 98.94% | 99.45% | 99.86% |
| coverage +/-4096 | 99.74% | 99.83% | 99.97% |
| sub-penny adds | 4 (0.0016%) | 2 (0.0011%) | 5 (0.0029%) |
| peak live levels | 2,057 | 775 | 1,798 |
| peak live orders | 7,073 | 2,145 | 13,843 |
| price range | $0.0001 - $199,999.99 | same | same |
| crossed states | 0 | 0 | 0 |

These decide the ladder geometry; the reasoning is in DESIGN.md section 15.
They will be re-measured on the complete file, which is still downloading.

## Not measured yet

Listed so that their absence is explicit rather than quiet.

- Parse throughput on its own, without a decompressor in the loop.
- All of the above over the complete session rather than a 490 MB prefix.
- Book update latency: p50, p99, p99.9, for each of the three implementations.
- Book update throughput for each of the three implementations.
- Cache-miss and branch-miss counters explaining the ratios between them.
  **Linux box.**
- Fine-grained p50. **Linux box.**
- Matching engine throughput and latency.
- Before/after for the two bottlenecks found by profiling.
