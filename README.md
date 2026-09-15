# itch-exchange

A NASDAQ ITCH 5.0 feed parser, limit order book, matching engine and
deterministic replay harness, in C++20.

Work in progress. This README grows as the pieces land, and it does not claim
anything that has not been measured — see NUMBERS.md, which lists what has been
measured and what has not.

## Built so far

- Four build configurations: `release`, `release-checked`, `asan-ubsan`, `tsan`.
- `core/`: big-endian wire loads, three-level assertions, a deterministic digest.
- A dependency-free test harness. 19 tests, green under all four configurations.

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
