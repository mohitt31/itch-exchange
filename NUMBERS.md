# Numbers

Every number in the README appears here with the command that produced it, the
machine it ran on, and the build flags. Nothing in this file is an estimate.
Where something has not been measured yet it says so.

`bench/reproduce.sh` regenerates every number here in one command.

---

## How to read the absolute numbers

Every figure here was measured with **Low Power Mode off**, and that is
load-bearing. The same binary on the same input:

| | low power mode off | low power mode on | ratio preserved |
|---|---|---|---|
| FlatBook | 64,702,920 | 33,545,534 | -- |
| AvlBook | 30,705,814 | 15,785,775 | 2.11x / 2.12x |
| MapBook | 16,973,716 | 8,335,226 | 3.81x / 4.03x |

**A 48% drop, hitting all three almost equally. The ratios survive; the
absolutes do not.**

It is Low Power Mode specifically, **not** the power source. This file
previously attributed the fast numbers to mains power. That was wrong and the
measurement said so: battery with the mode off gives 64.7M, slightly *faster*
than the run originally recorded as "mains". The source is not the variable.

Every benchmark prints the power state above its own result and warns when Low
Power Mode is on. If a number here does not reproduce, check that line first.

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

```
sha256 8c97b5b13bc451c012c2466fb7e258da134dab29aa47b67fe7b0088c78e870be
```

Recorded here and not only in `data/CORPUS.json`, which is gitignored along with
the corpus itself -- a hash that only exists next to the file it verifies cannot
verify a copy made anywhere else.

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
./build/release/bench/bench_book --only flat --symbol QQQ --runs 5 data/01302019.NASDAQ_ITCH50.gz
./build/release/bench/bench_book --only avl  --symbol QQQ --runs 5 data/01302019.NASDAQ_ITCH50.gz
./build/release/bench/bench_book --only map  --symbol QQQ --runs 5 data/01302019.NASDAQ_ITCH50.gz
```

Machine: Apple M4, macOS 26.5, Apple clang 21.0.0.
Build: `release` preset, `-O3 -DNDEBUG -mcpu=native`, `ITCH_INVARIANT_LEVEL=0`.
Workload: QQQ's **3,448,973** real book operations over the complete session
(1,623,619 adds, 1,577,106 deletes, 189,321 replaces, 55,963 executions,
2,964 cancels).

### Throughput -- reliable

**Each implementation measured alone in its own process**, seven rounds.

| | median ops/s | ns/op | slowest round | fastest round | vs FlatBook |
|---|---|---|---|---|---|
| **FlatBook** | **64,702,920** | **15.5** | 64,597,237 | 65,077,418 | 1.00x |
| AvlBook | 30,705,814 | 32.6 | 30,670,430 | 30,731,794 | **2.11x** |
| MapBook | 16,973,716 | 58.9 | 16,100,943 | 16,993,188 | **3.81x** |

`AvlBook` differs from `FlatBook` in exactly one thing: the price-to-level
structure. Same pools, same intrusive queues, same order index, an AVL tree over
a node pool instead of the tick-indexed ladder. So **2.11x is what the ladder
bought**. `MapBook` is the textbook implementation and changes everything at
once, so **3.81x is what the whole design bought**.

**Why isolated and not interleaved.** Running all three in one process,
interleaved, gives each the same conditions -- but every absolute comes out
roughly half of the isolated figure, because MapBook's allocations evict the
other two's working sets between rounds. The ratios survive almost unchanged, so
the comparison was never wrong; the absolutes were. `bench_book` without
`--only` still runs the interleaved mode, because that is where all three are
required to produce the same book digest.

All three produce book digest `a0ed37f623c3aa3f`; the interleaved mode refuses
to report if they disagree.

### Latency p50 -- reliable, with its range

Open loop at 50% of the slowest implementation's saturation, so none is backed
up. Measured timer floor 41 ns, clock read about 15 ns, both reported by the
benchmark at startup. Across four invocations:

| | observed p50 (ns) | typical |
|---|---|---|
| harness floor | at or below the 41 ns timer floor | -- |
| FlatBook | 67, 70, 68, 102 | **~70** |
| AvlBook | 103, 106, 104, 123 | **~105** |
| MapBook | 160, 155, 159, 154 | **~156** |

p50 (~70 ns) is four times the throughput figure (16.8 ns/op) and both are
correct: the throughput number is amortised across a pipelined loop, while the
latency number contains a 15 ns clock read and is quantised to 41 ns. The true
per-operation cost is nearer the throughput figure; p50 is an upper bound on it.

### Latency p99 and beyond -- NOT reliable on this machine, with proof

FlatBook's p99, same binary, same input, four consecutive invocations:

| run | FlatBook p99 |
|---|---|
| 1 | 202 ns |
| 2 | 37,085 ns |
| 3 | 168 ns |
| 4 | **5,500,354 ns** |

**A 30,000x range for the same code on the same data**, while throughput over
the same runs varied by 1%. In other runs the *empty* pacing loop's own p99 was
454 ns against 70-85 ns elsewhere.

macOS has no `isolcpus`, no `nohz_full` and no way to pin a thread to a core.
**No p99 or p99.9 figure is quoted from this machine anywhere in this project.**

## Parser, on its own

```
./build/release/bench/bench_parse --runs 5 data/01302019.NASDAQ_ITCH50.gz
```

2,147,483,615 bytes inflated into memory before timing (the full session is
10.5 GB, which does not belong in RAM on a 16 GB machine), trimmed to a frame
boundary. **69,718,789 messages.** No file read and no decompressor in the
timed region.

| | msg/s | GB/s |
|---|---|---|
| frame only | **464,147,664** | 14.30 |
| frame + decode | **218,675,589** | 6.74 |

"Frame only" walks the length prefixes and validates each against the type
table. "Frame + decode" additionally reads every field a book builder uses, into
an accumulator the benchmark checks is non-zero.

Sanity check, because 449 M msg/s is near the range where a parser benchmark is
usually measuring nothing: the buffer averages 30.8 bytes per message, so
13.83 GB/s is **memory bandwidth on an in-memory buffer, not disk** -- no SSD is
involved. At roughly 4 GHz that is about 9 cycles per message for a 2-byte
big-endian load, a byteswap, three compares and a pointer advance, and about 19
cycles with five fields decoded. Both are plausible for a loop with a
well-predicted branch, and the message count is derived from a separate counting
pass over the same buffer.

For contrast, `itch_stats` reports 14.4 M msg/s over the gzip corpus. The
difference between that and 449 M is almost entirely inflate.

## Order pool

```
./build/release/bench/bench_pool --runs 5
```

One free plus one allocate per cycle, 4,000,000 cycles per round, against a
maintained live set. Interleaved rounds, median of 5.

| live objects | pool ns/cycle | new + delete ns/cycle | pool wins by |
|---|---|---|---|
| 8,192 | **4.63** | 29.17 | **6.30x** |
| 65,536 | **5.03** | 29.68 | **5.90x** |

Two honest qualifications. The pool runs with generation checking **on**,
because that is how it ships; the comparison is against a validating pool, not a
bare free list. And macOS's allocator has a per-thread cache for small
allocations that this pattern suits well, so 5x here is not comparable to
figures quoted against glibc.

The two working-set sizes are there because a small one flatters `malloc`: the
gap narrows from 5.31x to 4.35x as the live set grows past the cache, which is
the opposite of what a too-small benchmark would show.

### Prefault

| | minor faults |
|---|---|
| during pool construction (the prefault itself) | 513 |
| touching all 262,144 slots afterwards | **0** |

Zero is the number the prefault claim makes, so it is checked rather than
asserted. The first version of this measured 32 faults; they were the harness's
own handle vector, not the pool, and the vector is now allocated and touched
before the measurement window opens.

## Price ladder: what each path costs

```
./build/release/bench/bench_ladder --runs 5
```

1,277 levels in the window (the measured QQQ peak) in every row; only the
overflow population changes. 20,000,000 lookups per round.

| overflow levels | window ns | overflow ns | fallback slower by |
|---|---|---|---|
| 64 | 0.90 | 4.30 | 4.79x |
| **544** (measured QQQ) | **0.90** | **6.92** | **7.72x** |
| 4,096 | 0.89 | 8.21 | 9.20x |
| 32,768 | 0.90 | 11.69 | 13.04x |
| 262,144 | 0.89 | 15.44 | 17.28x |

**The window column does not move.** 0.90 ns regardless of how much is in the
overflow map -- a bitset test and an array load, which is what O(1) means here.
The map grows as log n, from 4.36 to 16.56 ns.

At the measured overflow population the fallback is 7.85x slower, and it takes
0.80% of lookups (see the profiling section).

Two harness bugs were fixed to get this number, both recorded in DESIGN.md
because each produced a plausible wrong answer: indexing the price sequence with
`%` put an integer division in both paths and collapsed the ratio to 1.24x, and
the far prices underflowed `Price` at large overflow counts, became the best bid
and dragged the window off the near levels. The benchmark now checks that the
levels landed where it intended before timing anything.

## Cancel from the middle of a queue is O(1)

```
./build/release/bench/bench_cancel --reps 300
```

One price level, all orders at the same price, cancelling at a given position.
Mean ns over 300 rebuild-and-cancel repetitions.

| depth | head | 25% | middle | tail |
|---|---|---|---|---|
| 64 | 21.5 | 21.1 | 20.7 | 17.5 |
| 1,024 | 14.7 | 14.4 | 14.2 | 13.6 |
| 16,384 | 21.0 | 18.2 | 19.1 | 15.3 |
| 100,000 | **63.6** | 22.5 | 23.9 | 17.9 |

Flat in both depth and position, at 100,000 deep as at 64. That is the O(1)
claim measured rather than asserted.

The 63.6 ns in the bottom-left cell is the one value that moves, and it is a
cold-cache effect rather than a walk: the head of a 100,000 deep queue was
inserted 100,000 allocations ago and its record has been evicted. Four times the
flat cost, not four thousand.

The same benchmark under identity hashing is how the order index's unbounded
deletion was found -- 54,984 ns to cancel from the head of a 100,000 deep level.
DESIGN.md section 22 has the account.

## Order record size

```
ITCH_ORDER_PAD_BYTES=N, same workload, nothing else changed
```

| sizeof(Order) | per 128 B line | power of two | ops/s |
|---|---|---|---|
| **32** | 4 | yes | **59,447,200** |
| 40 | 3 | no | 56,587,736 |
| 56 | 2 | no | 57,262,185 |
| **64** | 2 | yes | **59,041,960** |

Power-of-two sizes beat non-power-of-two by 2-4%. Density does not matter:
64 bytes at two per cache line is as fast as 32 at four. The access pattern is a
random lookup through a handle, never a scan of adjacent orders. DESIGN.md
section 21 records that half of the original rationale was wrong.

## Order reference hash

Steady state, real workloads, identity versus splitmix64:
QQQ **+6.91%**, AMD **+3.22%**, AAPL **+7.88%** in identity's favour, despite
identity doing three to four times more probes per operation.

Deletion, `bench_cancel`, cancelling from the head:

| depth | identity | splitmix64 |
|---|---|---|
| 1,024 | 1,220 ns | 16 ns |
| 16,384 | 8,843 ns | 21 ns |
| 100,000 | **54,879 ns** | **26 ns** |

Sequential keys form one unbroken probe cluster and backward-shift deletion
walks it. Deletes are 43% of this feed. **splitmix64 ships.**

## Order index sizing

The throughput optimum is a load factor, not a size, confirmed independently on
two symbols with a 5.6x difference in peak live orders.

QQQ (7,679 peak live):

| entries | load factor | ops/s |
|---|---|---|
| 16,384 | 43.2% | 18,836,147 |
| 32,768 | 21.6% | 23,466,717 |
| 65,536 | 10.8% | 25,875,752 |
| **131,072** | **5.4%** | **26,974,316** |
| 262,144 | 2.7% | 24,844,157 |
| 524,288 | 1.3% | 20,698,310 |

AAPL (42,774 peak live):

| entries | load factor | ops/s |
|---|---|---|
| 131,072 | 32.6% | 37,168,362 |
| 262,144 | 16.3% | 44,490,716 |
| **524,288** | **8.2%** | **47,993,190** |
| 1,048,576 | 4.1% | 43,600,107 |

Both peak between 5% and 9%, and both sides of the optimum are worse.
`index_entries_for(peak)` encodes it.

## Profiling: before and after

Two bottlenecks found by profiling. Reasoning in DESIGN.md section 20.

| | before | after | |
|---|---|---|---|
| index sized from pool capacity, not live set | 20,010,189 ops/s | 26,492,637 | **1.32x** |
| handle validation: 4 branches -> 2 | 26,811,927-27,306,964 | 29,717,177-30,098,291 | **1.11x** |

Both measured on the 475k-operation workload they were found with.

A third change was tried and **reverted**: slimming the assertion call sites to
let `deref_checked` inline showed 4.5% on one run, then overlapping ranges over
three more runs of each version, with the same out-of-line symbol count either
way. It was noise, and is recorded as noise rather than kept with a number.

## Matching engine

```
./build/release/bench/bench_engine --runs 5 --orders 400000
```

The engine's cost depends on how much matching it does, so aggression is a
parameter and the **fill count is reported next to every throughput figure**.
A flow that never crosses measures the add path and calls it matching.

| flow | orders/s | ns/order | fills | rested |
|---|---|---|---|---|
| 0% aggressive | 24,804,278 | 40.3 | **0** | 400,000 |
| 5% | 23,602,766 | 42.4 | 31,366 | 380,176 |
| 25% | 20,920,137 | 47.8 | 144,699 | 311,610 |
| 60% | 16,594,756 | 60.3 | 294,507 | 233,293 |

Monotonic: more matching, slower. The 0% row is the control -- zero fills, every
order rests -- so it is the pure add path with the matching check on top, and
60% aggression costs 50% more per order than that.

Eight participants with self-trade prevention set to cancel-newest, so the STP
check is in the measured path.

## Order field ordering

```
ITCH_ORDER_NAIVE_LAYOUT=1, same workload, nothing else changed
```

| layout | sizeof(Order) | padding | ops/s |
|---|---|---|---|
| packed (widest first) | **32** | 0 | 33,395,512 / 33,774,746 |
| naive (as first written) | 48 | 12 bytes | 31,999,192 / 32,419,044 |

**48 bytes to 32 with no field removed, for about 4% of the time.** The memory
is the better half of that trade; see DESIGN.md section 24.

## reproduce.sh

Every number above comes from one run of `bench/reproduce.sh --rounds 7`,
exit status 0, nine stages:

| stage | result |
|---|---|
| feed statistics | 368,366,634 messages, 0 malformed |
| price distribution | QQQ, SPY, AMD |
| three-way book, isolated | above |
| three-way book, interleaved | digest `a0ed37f623c3aa3f`, all three agree |
| parser | above |
| order pool | above, 0 minor faults after prefault |
| ladder paths | above |
| cancel is O(1) | above |
| matching engine | above |
| deterministic replay | digest `6344a2790a894bc5`, **10 runs identical** |
| tests, four configurations | 12/12 each |

Also verified on a **clean checkout**: `git clone`, symlink the corpus,
`./bench/reproduce.sh --rounds 3`. Nine stages, exit status 0, four
configurations green, ten-run replay identical. The only step not exercised
there is the corpus download, which was symlinked rather than refetched.

## The Linux half

```
tools/linux_counters.sh data/01302019.NASDAQ_ITCH50.gz QQQ 7
```

Before anything else on a new machine, two minutes:

```
gcc -O1 -o probe tools/probe_counters.c && ./probe
```

It opens a software event and four hardware events and says which of three
things is true: counters available, a permission problem
(`kernel.perf_event_paranoid`), or no PMU exposed at all -- the last being what a
VM usually looks like. Inside Docker Desktop on this Mac it reports the third.
Run it before downloading a 4.7 GB corpus onto a machine that cannot count.

Two measurements this machine cannot produce, packaged as one command so they
are a matter of access rather than of work:

1. **Counters** behind the 2.11x and 3.81x ratios. Those ratios are measured;
   the mechanism is currently reasoned, not counted. This is the one place the
   project claims more than it has shown.
2. **p99 and p99.9** on an isolated core.

The script reports whether `isolcpus` and `nohz_full` are actually in effect
rather than assuming it, and warns that the tail is not quotable if they are
not. It also re-runs the replay, whose digest must match the macOS one --
a different answer on a different architecture would mean replay depends on
something it should not.

CI already builds and tests all four configurations on ubuntu-latest x86-64, so
the code is known to work there; only the measurements are outstanding.

## Not measured yet

Listed so that their absence is explicit rather than quiet.

- Book update p99 and p99.9. Measured here but **scheduler-dominated and not
  quotable**; see above. **Linux box**, with `isolcpus` and `nohz_full`.
- Cache-miss, branch-miss and TLB counters explaining the **2.11x and 3.81x**
  ratios. The ratios are measured; the mechanism behind them is currently
  reasoned, not counted. **Linux box** -- `tools/linux_counters.sh` runs it.

That is the whole list. Everything else this file once deferred has since been
measured:

- ~~Matching engine throughput~~ -- measured, see above, reported against the
  fill count at four aggression levels.
- ~~Parser throughput on its own~~ -- measured, 464M msg/s framing.
- ~~The 24-byte order record~~ -- attempted and found unreachable: the stored
  reference is load-bearing for index erase and for the canonical digest.
  DESIGN.md section 21.
- ~~p50 latency~~ -- measured and quotable.

p99 and p99.9 are the other Linux item, recorded in their own section above with
the evidence for why they are not quotable from a Mac rather than left here.
