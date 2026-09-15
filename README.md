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

On an Apple M4, against 475,247 real QQQ book operations. Every number here is
reproduced by `bench/reproduce.sh`; the full conditions are in NUMBERS.md.

| | throughput | ns/op | p50 latency |
|---|---|---|---|
| FlatBook | 20.0M ops/s | 50.0 | 76 ns |
| AvlBook | 13.7M ops/s | 72.9 | 109 ns |
| MapBook | 8.7M ops/s | 114.3 | 155 ns |

`AvlBook` differs from `FlatBook` in one thing only -- an AVL tree over a node
pool instead of the tick-indexed ladder, with the same pools, queues and order
index -- so **1.46x is what the ladder bought**. `MapBook` is the textbook
implementation and changes everything at once, so **2.29x is what the whole
design bought**.

**There is no p99 figure here.** The benchmark measures its own floor by running
the identical loop with no book operation in it, and on macOS that floor moves
by 6x between identical runs. p99 and p99.9 need a machine with core isolation;
until that runs, the number does not exist. NUMBERS.md records the observed
ranges as evidence for why, not as results.

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
