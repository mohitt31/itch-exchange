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

Apple M4, against **3,448,973 real QQQ book operations** from a complete NASDAQ
session. Every number here has a command in NUMBERS.md; `bench/reproduce.sh`
regenerates all of them.

### Book updates

| | throughput | ns/op | p50 latency |
|---|---|---|---|
| **FlatBook** — tick-indexed ladder | **59.5M ops/s** | **16.8** | ~70 ns |
| AvlBook — AVL over a node pool | 29.2M ops/s | 34.2 | ~105 ns |
| MapBook — `std::map` + `std::list` | 16.8M ops/s | 59.5 | ~156 ns |

`AvlBook` differs from `FlatBook` in **one thing only**: the price-to-level
structure. Same pools, same intrusive queues, same order index. So **2.04x is
what the ladder bought**. `MapBook` is the textbook implementation and changes
everything at once, so **3.54x is what the whole design bought**. Both are
reported because they answer different questions.

### Parser, with no decompressor in the loop

| | msg/s | GB/s |
|---|---|---|
| frame + validate | 449M | 13.8 |
| frame + decode every field a book reads | 212M | 6.5 |

In-memory buffer, so the GB/s is memory bandwidth, not disk.

### The pieces

| | |
|---|---|
| ladder window lookup | **0.90 ns**, flat regardless of overflow size |
| overflow map lookup | 7.10 ns at the measured population (7.85x) |
| pool allocate + free | 5.56 ns, **5.31x** faster than `new`/`delete` |
| page faults on the hot path after prefault | **0** |
| cancel from the middle of a 100,000-deep queue | 17 ns, same as at depth 64 |

### There is no p99 figure here, on purpose

The benchmark measures its own floor by running the identical loop with no book
operation in it. On this machine FlatBook's p99 came out as 202 ns, 37,085 ns,
168 ns and 5,500,354 ns across four consecutive runs of the same binary on the
same data — a 30,000x range — while throughput over those same runs varied by 1%.

macOS has no `isolcpus` and no `nohz_full`. p99 and p99.9 need a machine with
core isolation; until that runs, the number does not exist. NUMBERS.md records
the spread as evidence for why, not as a result.

## Validated against the real feed

```
./build/release/apps/itch_stats data/01302019.NASDAQ_ITCH50.gz
```

368,366,634 messages, 10.5 GB of payload, **zero malformed frames**. The framing
layer checks each message's length prefix against the length declared for its
type byte, so a clean pass is every one of the 23 layout structs validated
against reality 368 million times.

```
./build/release/apps/itch_replay --symbol QQQ --runs 10 data/01302019.NASDAQ_ITCH50.gz
```

The replay journal's digest is identical across ten runs, across all three book
implementations, and across the `release`, `release-checked` and `asan-ubsan`
builds. A digest that survives both `-O3` and ASan is evidence that no
optimisation changed the answer. Zero crossed states over the session.

## Three findings worth the reading

**The stub quotes decide the ladder.** Every symbol carries orders at $0.0001
and $199,999.99, posted once and left for the session. Under a direct-mapped
scheme they alias into the hot window and squat there all day. That, not the
distance histogram, is what ruled the design out. (DESIGN.md §15)

**Half my reason for a 32-byte order record was wrong.** Power-of-two sizing is
worth 2–4%; four-per-cache-line is worth nothing, because every access is a
random lookup through a handle and never a scan. Measured with a padding knob
that changes the size and nothing else. (§21)

**The obvious hash was faster and still wrong.** Identity hashing beat splitmix64
by 3–8% on every real symbol. Then a benchmark written to test something else
entirely — whether cancel is O(1) — showed it takes 54,878 ns to erase from the
front of a 100,000-key cluster, against splitmix64's 26 ns, because sequential
keys make backward-shift deletion walk the whole cluster. Deletes are 43% of
this feed. (§22)

## Building

```sh
cmake --preset release-checked
cmake --build --preset release-checked
ctest --preset release-checked
```

Presets are `release`, `release-checked`, `asan-ubsan` and `tsan`. ASan and TSan
cannot coexist, hence the separate configurations.

## Design

DESIGN.md carries the decisions: what was chosen, what was rejected, and why.
NUMBERS.md carries every measurement with the command that reproduces it.
