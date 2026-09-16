# itch-exchange

A NASDAQ ITCH 5.0 feed parser, limit order book, matching engine and
deterministic replay harness, in C++20.

Work in progress. This README grows as the pieces land, and it does not claim
anything that has not been measured — see NUMBERS.md, which lists what has been
measured and what has not.

## Built so far

- All 23 ITCH 5.0 message types, generated from the specification: 191 fields,
  451 `static_assert`s on size and offset. Validated against 40.4 million real
  messages.
- Framing that validates rather than only delimits, over three input paths
  (mapped, streamed, gzip), checked against each other.
- Three limit order books behind one interface, differential-tested against a
  fourth deliberately naive model.
- A three-way benchmark on real feed operations.

## Results

Apple M4, **Low Power Mode off**, against **3,448,973 real QQQ book operations**
from a complete NASDAQ session. Every number comes from one run of
`bench/reproduce.sh`, which also works on a clean checkout. Conditions are in
NUMBERS.md.

### Book updates

| | throughput | ns/op | p50 latency |
|---|---|---|---|
| **FlatBook** — tick-indexed ladder | **64.7M ops/s** | **15.5** | ~70 ns |
| AvlBook — AVL over a node pool | 30.7M ops/s | 32.6 | ~105 ns |
| MapBook — `std::map` + `std::list` | 17.0M ops/s | 58.9 | ~156 ns |

`AvlBook` differs from `FlatBook` in **one thing only**: the price-to-level
structure. Same pools, same intrusive queues, same order index. So **2.11x is
what the ladder bought**. `MapBook` is the textbook implementation and changes
everything at once, so **3.81x is what the whole design bought**. Both are
reported because they answer different questions.

### Parser, with no decompressor in the loop

| | msg/s | GB/s |
|---|---|---|
| frame + validate | 464M | 14.3 |
| frame + decode every field a book reads | 219M | 6.7 |

In-memory buffer, so the GB/s is memory bandwidth, not disk.

### Matching engine

Cost depends on how much matching it does, so the fill count is reported beside
every figure. A flow that never crosses measures the add path and calls it
matching.

| flow | orders/s | ns/order | fills |
|---|---|---|---|
| 0% aggressive | 24.8M | 40.3 | **0** |
| 25% | 20.9M | 47.8 | 144,699 |
| 60% | 16.6M | 60.3 | 294,507 |

### The pieces

| | |
|---|---|
| ladder window lookup | **0.90 ns**, flat regardless of overflow size |
| overflow map lookup | 6.92 ns at the measured population (7.72x) |
| pool allocate + free | 4.63 ns, **6.30x** faster than `new`/`delete` |
| page faults on the hot path after prefault | **0** |
| cancel from the middle of a 100,000-deep queue | 23.9 ns, same as at depth 64 |
| `sizeof(Order)` | 32 bytes, 0 padding (48 before reordering) |

### Two things about these numbers

**Low Power Mode halves them.** The same binary measures 33.5M rather than 64.7M
with it on — a 48% drop that hits all three implementations almost equally and
leaves the ratios intact. Every benchmark prints the power state above its
result. It is the mode specifically, not the power source; this README used to
say "mains power" and the measurement showed that was the wrong variable.

**There is no p99 figure here, on purpose.** The benchmark measures its own floor
by running the identical loop with no book operation in it. FlatBook's p99 came
out as 202 ns, 37,085 ns, 168 ns and 5,500,354 ns across four consecutive runs of
the same binary on the same data — a 30,000x range — while throughput over those
same runs varied by 1%. macOS has no `isolcpus` and no `nohz_full`. p99 and
p99.9 need a machine with core isolation; until that runs, the number does not
exist.

## Building

```sh
cmake --preset release-checked
cmake --build --preset release-checked
ctest --preset release-checked
```

Presets are `release`, `release-checked`, `asan-ubsan` and `tsan`. ASan and TSan
cannot coexist, hence the separate configurations.

Before pushing:

```sh
./tools/check_gcc.sh                      # every TU under GCC at -O3, full warnings
./tools/check_headers_selfcontained.sh    # every header compiles alone
./tools/check_noexcept_asserts.sh         # no noexcept function contains an assertion
./tools/check_no_float.sh                 # no floating point below apps/
```

CI runs all four configurations on macOS and Ubuntu, plus those checks and a
regeneration diff of the wire headers.

## Design

DESIGN.md carries the decisions: what was chosen, what was rejected, and why.
NUMBERS.md carries every measurement with the command that reproduces it.
