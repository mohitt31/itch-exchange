# Design notes

Written as each decision is made, not reconstructed afterwards. Each entry says
what was chosen, what was rejected, and why. Where a decision was settled by
measurement, the number is in NUMBERS.md and referenced here.

---

## 1. Two front doors, one book

ITCH is a market data feed. It reports matches that have already happened at
NASDAQ; it does not ask us to decide anything. The matching engine is the
opposite: it consumes orders and decides fills.

So the project has two entry paths sharing one data structure:

- **Replay path** — ITCH messages mutate book state. Executions are *applied*.
- **Exchange path** — synthetic orders enter the matching engine, which decides
  fills and mutates the same book type.

Conflating these is the standard error in this project. Keeping them separate is
also what makes the differential tests meaningful: the replay path has an
external ground truth (the exchange's own output), the exchange path has an
internal one (three book implementations agreeing).

## 2. Assertions are on in release

`ITCH_ASSERT` compiles into the release build. The conditions it guards — a
stale order handle, an exhausted pool, an impossible message length — have no
safe continuation. Turning them off does not make the program faster in any way
that matters; it makes a corrupt book silent.

Three levels, so the cost is proportional to what is being checked:

| Macro | On at level | Cost |
|---|---|---|
| `ITCH_ASSERT` | always, release included | one predictable branch |
| `ITCH_INVARIANT` | >= 1 | O(1) structural check |
| `ITCH_SLOW_INVARIANT` | >= 2 | O(n) re-derivation of a whole structure |

This is why there are four build configs rather than three. `release` is level 0
and is what the benchmarks measure; `release-checked` is level 1 and is what the
long replay runs under, so that invariants are asserted *during* a run at
realistic speed rather than only under a sanitizer at 20x slowdown.

**Rejected:** the standard `assert()` / `NDEBUG` pairing. One knob for three
very different costs means either the O(n) checks run in production or the O(1)
ones do not run at all.

The assert handler is replaceable. That is not a general-purpose hook — it
exists so `ITCH_REQUIRE_ASSERT(stmt)` can be an ordinary test case. Testing that
a stale handle is *detected* is as important as testing that a valid one works,
and the alternative is fork-based death tests.

## 3. The wire structs are not the parse path

`wire/messages.hpp` will declare 22 packed structs with `static_assert` on
`sizeof` and every `offsetof`. The parser does **not** cast a `const std::byte*`
to them. That cast is undefined behaviour twice over — alignment and strict
aliasing — and UBSan reports it correctly, so it would mean either a broken
sanitizer config or a suppression.

Instead every field is read as:

```cpp
u64 ref = load_be<u64>(p + offsetof(AddOrderMsg, order_ref));
```

`load_be` is `memcpy` plus `__builtin_bswap`, which is fully defined and which
arm64 compiles to one unaligned load and one `rev`. The `offsetof` keeps a
single source of truth: the struct declaration and the parse path cannot drift
apart, which is the entire reason for declaring the structs.

The 48-bit ITCH timestamp gets its own `load_be48`, built from a 16-bit and a
32-bit load. The tempting version — an 8-byte load at `p - 2`, shifted — reads
out of bounds at the start of a buffer, and ASan is right to complain.

**Rejected:** accessor methods on the packed structs (same UB, better hidden);
`std::start_lifetime_as` (C++23, and this is a C++20 project).

## 4. The digest hashes fields, never bytes

The replay determinism check hashes a canonical event stream. It feeds named
fields one call at a time. It never hashes raw struct bytes.

Struct padding is uninitialised. Hashing it gives a digest that can be stable
under one compiler and different under another, or stable in `release` and
different under ASan. That is precisely the class of bug the determinism check
exists to catch, so the check must not contain it.

`Digest` is `constexpr`, and one test evaluates it at compile time and compares
against the runtime result. If anything address-dependent or build-dependent
ever leaks into the digest, that test stops compiling or stops matching.

**Rejected:** `std::hash` (unspecified, may differ between standard library
versions, explicitly not required to be stable); FNV-1a byte-at-a-time (fine,
but the field-wise splitmix64 finaliser is both faster per field and better
distributed on small integers, which is nearly all this project feeds it).

## 5. Strong types only where a mixup compiles

Most quantities are plain integer aliases: `Price`, `Qty`, `OrderRef`. Exactly
one strong type exists — `Ticks`, the ladder index.

The reasoning is narrow. A strong type earns its verbosity when the mistake it
prevents would otherwise compile cleanly *and* produce a plausible wrong answer.
Confusing a raw ITCH price (1/10000 USD) with a ladder tick index does exactly
that: both are integers, both are in range, and the result is a silently wrong
book. Confusing a `Qty` with a `Price` does not survive five minutes of testing.

**Rejected:** strong types everywhere. The cost is not performance, it is that
every arithmetic expression grows a `.v` and reviewers stop reading.

## 6. No floating point below apps/

ITCH prices are integers with four implied decimals and they stay integers
through the book, the matching engine and the journal. A single `double` would
make replay output depend on optimisation flags and on FMA contraction, which
destroys the determinism guarantee.

Enforced twice: `-Wdouble-promotion` as the compiler-side tripwire, and
`tools/check_no_float.sh` in CI for the source side, because a deliberate
`double x` compiles without any warning at all.

## 7. Warning set

`-Wall -Wextra -Wpedantic -Werror` plus `-Wshadow`, `-Wconversion`,
`-Wsign-conversion`, `-Wold-style-cast`, `-Wnon-virtual-dtor`, `-Wformat=2`,
`-Wdouble-promotion`.

`-Wconversion` and `-Wsign-conversion` are the two that cost real effort in a
codebase doing integer arithmetic on mixed widths. They are on deliberately: a
silent narrowing in price or quantity arithmetic is a wrong book, and this is
the only place the compiler will ever point at it for free.

## 8. Test harness, no dependency

No gtest, catch2 or doctest is installed on this machine, and the harness needs
two things a generic framework does not give directly:

- `ITCH_REQUIRE_ASSERT(stmt)` — asserting that a misuse is *detected*.
- `ITCH_TEST_CONTEXT(note)` — a scoped note printed on failure, so a seeded
  property test reports the seed that failed and nothing has to be
  reconstructed by hand.

180 lines, and the repo has zero test dependencies. The harness is itself
verified negatively: a throwaway suite of deliberately failing cases confirms
that CHECK reports and continues, REQUIRE aborts the case, a missing assertion
is caught, context is printed, and the process exits non-zero.

**Rejected:** vendoring doctest's single header. It would have been defensible;
the deciding factor was `REQUIRE_ASSERT`, which would have needed a custom
extension anyway.

## 9. The wire header is generated from the specification, not typed

`include/itch/wire/messages.hpp` is produced by `tools/gen_wire.py` from
`tools/itch50_fields.json`, which `tools/extract_spec.py` pulls out of the
official PDF (sha256 `45e0531d...`, recorded in the generated header). 23
structs, 191 fields, 451 `static_assert`s.

Typing 191 offsets by hand is a transcription exercise with a high chance of one
silent error, and the error would be in the one place nothing else can catch it.
Generating them means the failure mode is a parser crash on the first real
message rather than a field that is quietly off by one for the whole day.

The generated header is committed so that building needs no Python. CI
regenerates it and diffs, so it cannot drift from the extracted tables.

### The structs are all byte arrays

Every member is `Bytes<N>` or `Alpha<N>`. That makes `alignof` 1 by construction
and padding impossible, and both facts are asserted rather than assumed:

```cpp
static_assert(sizeof(AddOrder) == 36);
static_assert(alignof(AddOrder) == 1);
static_assert(std::is_standard_layout_v<AddOrder>);   // offsetof needs this
static_assert(offsetof(AddOrder, price) == 32);
static_assert(sizeof(AddOrder::price) == 4);
```

**Rejected:** packed structs with native integer members. It invites reading
through the struct, which is the undefined behaviour described in section 3, and
`#pragma pack` then hides the alignment question instead of answering it.

### Three independent checks on the same layout

The asserts alone would only prove the header is self-consistent. Three separate
sources have to agree:

1. **The extractor**, mechanically, from the PDF.
2. **A hand-written table in `tests/test_wire.cpp`**, listing all 23 sizes typed
   from the specification independently of the extractor. If the extractor
   mis-parses a table, the two disagree and the test fails.
3. **The real feed** (slice 4). NASDAQ's BinaryFILE framing prefixes every
   message with its length, so asserting `framed_length == message_length(type)`
   over the whole file checks all 23 sizes against reality hundreds of millions
   of times.

The static_asserts were also verified to be non-vacuous: injecting one extra
byte into `AddOrder` fails the build on the size assert and on every downstream
offset assert, as it should.

### What the spec actually contains

The current specification (April 2023) defines **23** message types, not 22.
`'O'`, Direct Listing with Capital Raise, was added after the commonly cited
list. All 23 are implemented.

Two inconsistencies in the spec's own tables, both handled explicitly in
`gen_wire.py`'s override table rather than silently:

- The Reg SHO table calls the common header's second field **Locate Code**;
  every other table calls the same field at the same offset **Stock Locate**.
  Normalised, because the parser reads that field before it knows which message
  it holds. The uniform-header test in `test_wire.cpp` is what found this.
- Roughly a dozen field names are truncated by the PDF's column width. Offsets
  and lengths are always taken from the extractor; only the names are completed
  by hand, and the override table makes every such completion visible.

## 10. Headers must compile on their own

`tools/check_headers_selfcontained.sh` compiles each header as its own
translation unit. This was not a hypothetical: `types.hpp` used `<=>` without
including `<compare>` and compiled anyway, because every existing translation
unit happened to include something that pulled it in first. It surfaced only
when the header was compiled alone. Now CI compiles all of them alone.

---

## Open, to be settled by measurement

These are recorded now so that the decision is visibly made by data and not by
preference. Each will be resolved in the slice named.

- **Price ladder scheme** (slice 7) — fixed absolute window, sliding window with
  rebase, or direct-mapped cache with an overflow hash. Pre-committed decision
  rule: if >= 99.9% of book updates land within +/- K ticks of the inside for
  K <= 2048, take the sliding window; if the distribution is heavy-tailed but
  sparse, take direct-mapped. Measured in slice 5.
- **Tick alignment** (slice 5) — the ladder must be indexed by penny, not by raw
  1/10000 units, or a +/- $5 window needs 5.1M slots instead of 512. Reg NMS
  Rule 612 forces displayed orders on stocks >= $1 to penny increments, so
  almost everything should be aligned, but sub-penny prices are legal below $1
  and appear in execute-with-price messages. The misalignment fraction gets
  measured, not assumed, and the remainder goes to the overflow hash.
- **Order record width** (slice 6) — 32 bytes as planned, versus 24 bytes by
  dropping the stored `OrderRef`. 32 is a power of two, so indexing is a shift
  rather than a multiply; 24 fits five per 128-byte line instead of four. Both
  get built behind the same interface and benchmarked.
- **Levels inline in the ladder, or behind a handle** (slice 7) — inline makes
  the ladder 128 KB per side and mostly empty; handles make it 16 KB, exactly
  one M4 page, with levels densely packed, at the cost of one extra dependent
  load. Benchmarked.
- **Order reference hash** (slice 8) — NASDAQ order references are
  near-sequential, which makes identity hashing plausible and possibly optimal
  for linear probing. Identity versus a splitmix64 finaliser, benchmarked.
