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

## 11. Two read paths, checked against each other

`MappedFile` maps the whole file and `FrameCursor` walks it with no copying at
all. `FrameReader<Source>` owns a buffer, refills it from a byte source and
moves the straddling tail to the front. Two shapes because two access patterns:
mmap is addressable up front, gzip and stdin are not.

The mapped path is what benchmarks use. Putting inflate inside a timing loop
would be measuring inflate.

They are not trusted to agree, they are tested to. `test_framing` runs the same
input through both at chunk sizes of 1, 3, 7, 64 and 4096 bytes, so every
message is forced to straddle a refill, and requires the frame sequences to be
byte-identical. `test_gzip` does the same through a real zlib stream.

The buffer must hold one whole framed message or a straddling message could
never be completed. `kMinBufferSize` is `kMaxMessageLength + 2`, and the
constructor asserts it rather than documenting it.

## 12. Framing validates, it does not just delimit

`FrameCursor` compares each message's two-byte length prefix against the length
declared for its type byte, and distinguishes four failures: a length that
disagrees with the type, an unknown type, a zero length, and a truncated tail.
It is a table lookup and a compare.

That check is what makes a full pass over the real feed a test of all 23 layout
structs, hundreds of millions of times, rather than a test of 23 `static_assert`s
that only prove the header agrees with itself.

`NeedMore` and `Malformed` are kept apart deliberately. A truncated tail is
expected -- the corpus is a byte range of a gzip stream and ends mid-message by
construction -- while a length that disagrees with its type means the stream is
lost. A malformed frame is not consumed, so the caller can report where it
happened instead of guessing.

## 13. Views decode one field at a time

`views.hpp` is generated alongside `messages.hpp`. A view is a bare pointer;
each accessor decodes exactly the field asked for, at `offsetof` on the layout
struct. A handler that only wants the order reference pays for one load and one
`rev`, not for a 36-byte decode.

Dispatch uses `if constexpr (requires { handler.on_add_order(v); })`, so a
handler implements only the messages it cares about. No virtual calls, no empty
overrides to inherit, and a handler that ignores 20 of the 23 types compiles to
a switch with 20 empty arms.

**Rejected:** a `std::variant` of decoded messages (copies every field, including
the ones nobody reads); a virtual handler interface (an indirect call per
message on the hottest loop in the program).

## 14. A permissive error path hid a real bug for an hour

Two bugs in the gzip source, and the second one is the one worth recording.

**The bug.** zlib's inflate state holds a back-pointer to the `z_stream` it was
initialised with, and `inflate()` rejects the stream when that pointer no longer
matches. So a `z_stream` cannot be moved by value. The move constructor copied
it, every `inflate` call returned `Z_STREAM_ERROR`, and the decompressor was
completely dead. The fix is a `unique_ptr<z_stream>`, so the object stays movable
while the address zlib knows about stays put.

**Why it was not obvious.** The read loop treated *any* non-`Z_OK` return as
"input must have been truncated, stop here", because the corpus is a truncated
gzip prefix and that case has to be tolerated. A totally dead decompressor
therefore looked exactly like a clean, empty file: no error, no exception, exit
status zero, a neatly formatted report of zero messages.

`allow_truncated` now forgives exactly one thing: the compressed input ending
before `Z_STREAM_END`. Every `inflate` error throws, whatever the flag says.
`Z_BUF_ERROR` with bytes still unread also throws, because no progress with
input available is not a refill condition -- and the old code would have spun on
it forever.

The lesson is the general one: an error path that is permissive because one
legitimate case needs it will swallow the illegitimate cases too. Forgive the
specific condition, never the category.

`test_gzip.cpp` now covers both. Restoring the permissive branch for one build
fails `gzip_corrupt_input_always_throws` and `gzip_bad_header_throws_immediately`,
which was checked rather than assumed.

## 15. The price ladder, decided by measurement

This is the decision the whole project turns on, and it was pre-committed in
section "Open, to be settled by measurement" before any data was seen. The rule
was: sliding window if 99.9% of insertions land within +/- K ticks for K <= 2048,
direct-mapped if the distribution is heavy-tailed but sparse.

The data says something the rule did not anticipate, so here is what was
measured and what it actually implies.

### What was measured

`apps/itch_histogram` over a 490 MB prefix of the 30 January 2019 session
(40.4 million messages, 03:03 to 09:52), for the three busiest symbols.
Distances are in pennies, measured at insertion against the same side's inside,
before the order is applied.

| | QQQ | SPY | AMD |
|---|---|---|---|
| adds measured | 251,286 | 189,464 | 175,078 |
| p50 | **0** | 1 | **0** |
| p90 | 9 | 17 | 67 |
| p99 | 2,138 | 797 | 1,068 |
| p99.9 | 6,378 | 5,299 | 2,156 |
| improving the inside | 6.53% | -- | -- |
| peak live levels | 2,057 | 775 | 1,798 |
| peak live orders | 7,073 | 2,145 | 13,843 |

Coverage by window half-width:

| window | QQQ | SPY | AMD |
|---|---|---|---|
| +/-16 | 92.03% | 89.82% | 81.80% |
| +/-256 | 93.41% | 94.35% | 94.82% |
| +/-1024 | 98.08% | 99.25% | 98.90% |
| **+/-2048** | **98.94%** | **99.45%** | **99.86%** |
| +/-4096 | 99.74% | 99.83% | 99.97% |

### Finding 1: half of all insertions land exactly on the inside

p50 is 0 or 1 tick on every symbol. The single hottest slot in the ladder is the
inside itself, and the array around it is what the next 40% of insertions touch.
This is the result that justifies a flat array at all.

### Finding 2: penny indexing works, and the alternative never would have

Sub-penny prices are **0.0016% of adds on QQQ, 0.0011% on SPY, 0.0029% on AMD** --
four, two and five orders respectively out of hundreds of thousands. Reg NMS
Rule 612 does what it says.

So the ladder is indexed by penny and the handful of sub-penny prices go to the
overflow map. Indexing by raw 1/10000 units instead would have needed 5.1 million
slots to span what 512 slots span now, for a 0.002% correctness gain that the
overflow map already provides.

### Finding 3: every symbol has permanent orders at absurd prices

All three symbols report the same price range: **$0.0001 to $199,999.99**. These
are stub quotes -- orders posted far from the market to satisfy two-sided
quoting obligations. They are real, they are in the feed, and crucially they are
posted once and left there.

**This is what rules out the direct-mapped design.** Under `slot = price_tick &
(N - 1)`, a stub quote at $199,999.99 aliases onto some slot inside the hot
window and, because it is never cancelled, squats there for the entire session.
Every access to the real price that maps to that slot falls through to the
overflow map, all day, for no reason a profile would make obvious.

A sliding window has no such failure: a price outside the window is simply not
in the array, so stub quotes live in the overflow map permanently and never
touch the flat storage at all. That is the correct place for them.

### Finding 4: a fixed absolute window is unworkable, but not for the expected reason

The inside travelled 301 ticks on SPY and 289 on AMD over the measured period --
small enough that a fixed window looks tempting. QQQ reports 4,402 ticks, which
is a pre-market artefact: before the session opens the book is nearly empty and
the "best bid" is a stub quote, so the range is measuring the stub, not the
market.

The real objection is simpler. A fixed window needs an anchor chosen before the
session, and nothing in the feed provides one that is not contaminated by
exactly that pre-market noise.

### Decision

**A sliding window of +/- 2048 ticks per side, plus an open-addressed overflow
map**, with the window rebased when the inside drifts past a threshold.

| | |
|---|---|
| slots per side | 4096 (+/- 2048 pennies = +/- $20.48) |
| bytes per side | 4096 x 4-byte level handle = **16 KiB, exactly one M4 page** |
| occupancy bitset | 4096 bits = 512 B = 4 cache lines per side |
| measured hit rate | 98.9% to 99.9% of insertions |
| everything else | overflow map, including all stub quotes and all sub-penny prices |

16 KiB is not a coincidence that was designed for and then confirmed; it is the
window size the coverage table pointed at, which happens to land on the page
size. The handle array for one side is one page and one TLB entry, and the live
level pool at peak (2,057 levels x 32 B = 64 KiB) sits inside the P-core's
128 KiB L1.

**Rejected: direct-mapped cache.** Finding 3. The stub quotes would poison it.

**Rejected: fixed absolute window.** Finding 4. No honest anchor exists.

**Rejected: +/- 4096 ticks.** It buys 0.1 to 0.8 percentage points of hit rate
for twice the footprint, pushing the handle array to two pages. The overflow map
handles that traffic at a cost the profile will show, and if it shows it
mattering the number can change -- with a measurement attached.

### Caveat, recorded rather than hidden

These figures come from a 490 MB prefix covering 03:03 to 09:52, so roughly the
pre-market and the first twenty minutes of the session. Peak live order counts
in particular will grow over a full day. The measurement will be re-run on the
complete file and this section updated; the window decision rests on the shape
of the distribution and on the stub quotes, neither of which a longer sample
changes.

---

## Open, to be settled by measurement

These are recorded now so that the decision is visibly made by data and not by
preference. Each will be resolved in the slice named.

- ~~**Price ladder scheme**~~ -- settled by measurement, section 15. Sliding
  window of +/- 2048 ticks plus an overflow map. Direct-mapped was rejected
  because every symbol carries permanent stub quotes that would squat on hot
  slots all session.
- ~~**Tick alignment**~~ -- settled by measurement, section 15. Sub-penny prices
  are 0.001% to 0.003% of adds, so penny indexing works and the remainder goes
  to the overflow map.
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
