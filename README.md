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

## Measured so far

Apple M4, against **3,448,973 real QQQ book operations** from a complete
NASDAQ session. Every number is reproduced by `bench/reproduce.sh`; the
conditions are in NUMBERS.md.

| | throughput | ns/op | p50 latency |
|---|---|---|---|
| **FlatBook** | **30.7M ops/s** | **32.5** | ~70 ns |
| AvlBook | 15.1M ops/s | 66.1 | ~105 ns |
| MapBook | 8.2M ops/s | 122.3 | ~156 ns |

`AvlBook` differs from `FlatBook` in one thing only -- an AVL tree over a node
pool instead of the tick-indexed ladder, with the same pools, queues and order
index -- so **2.03x is what the ladder bought**. `MapBook` is the textbook
implementation and changes everything at once, so **3.76x is what the whole
design bought**.

### There is no p99 figure here, on purpose

The benchmark measures its own floor by running the identical loop with no book
operation in it. On this machine FlatBook's p99 came out as 202 ns, 37,085 ns,
168 ns and 5,500,354 ns across four consecutive runs of the same binary on the
same data -- a 30,000x range, dominated by the macOS scheduler.

p99 and p99.9 need a machine with core isolation. Until that runs, the number
does not exist. NUMBERS.md records the observed spread as evidence for why.

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
