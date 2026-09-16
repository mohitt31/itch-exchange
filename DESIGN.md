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

The source check has already earned its keep: a `probes_per_op()` diagnostic
accessor on `OrderIndex` returned a `double`, and the check refused it. It was
harmless -- a counter ratio, never on the book path -- which is exactly the
reasoning that would let the next one in. The accessor now returns the two
counters and callers in `apps/` and `bench/` divide them.

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

## 16. Handles, generations, and what the detector actually guarantees

Orders and levels live in pools and are addressed by 32-bit handles: 24-bit slot
index, 8-bit generation. Handles rather than pointers because they are half the
size -- two of them sit in every order record, which is sized to fit four per
cache line -- because they survive the pool being reallocated, and because a
pointer cannot tell you its target has been freed.

Tagged, so `OrderHandle` and `LevelHandle` are different types. This is the
second place strong typing earns its keep by the rule in section 5: both are
u32 indices into a pool, both are in range, and confusing them produces a book
that looks entirely plausible.

### The generation's low bit is the live flag

Incremented on both allocate and free, so an odd generation means live and an
even one means free. One compare on the handle's own generation answers both
"is this the slot I was given?" and "is it still alive?", and it reads the byte
that is already on the cache line the caller is about to use.

**The cost, stated plainly:** the generation now wraps every 128 allocate/free
cycles on a slot rather than 256. A handle held across exactly 128 reuses is
accepted again.

That limit is not hypothetical and it is not hidden --
`pool_generation_wraps_without_quarantine` drives a one-slot pool through
exactly 127 cycles (still caught) and then 128 (accepted), so the boundary is
pinned by a test rather than by a comment. This is a bug detector, not a
security boundary.

**Mitigation:** the sanitizer builds hold 4096 freed slots in a quarantine
before they can be reused, so a stale handle names a slot whose generation has
certainly moved. That is a per-pool parameter, not a compile-time constant,
because a test that wants to force a slot straight back has to be able to turn
it off -- which is how the wrap test above is possible at all.

**Rejected:** widening to a 40-bit index and 24-bit generation. It removes the
wrap entirely and doubles the handle to 8 bytes, which pushes `Order` past 32
and out of four-per-cache-line. The measured peak was 13,843 live orders on the
busiest of three symbols; 24 bits addresses 16.7 million.

### The free list is intrusive without type punning

The usual intrusive free list writes an index over the slot's raw storage, which
is type punning and which UBSan is right to object to. Here the link lives in a
field the slot cannot be using while it is free: an order in the free list is in
no queue, so its forward queue link carries the free list link. Same field, same
type, zero extra bytes, fully defined.

`Level` cannot do this -- its queue links are in use whenever the level is -- so
it carries a dedicated 4-byte link. It still lands on 32 bytes.

### Prefaulting is about measurement, not speed

Every page is written once at construction. Written, not read: a read of
untouched anonymous memory can be served by the shared zero page and faults
again on the first write.

This is not a micro-optimisation. Without it, the first touch of each page
during a replay is a multi-microsecond outlier, and those outliers land in
exactly the percentile the project is meant to report honestly. Prefaulting
keeps p99.9 a measurement of the book rather than of the page allocator.

## 17. The ladder, built

`PriceLadder` implements what section 15 measured. Per side:

| piece | size | why |
|---|---|---|
| occupancy bitset | 4096 bits = 512 B = 4 cache lines | finding the next best after a level clears is a `clz`/`ctz` scan of u64 words, usually one word |
| slot array | 4096 x 4-byte handle = 16 KiB | one M4 page, one TLB entry |
| overflow | `std::map` | ordered, see below |

**The invariant that makes lookup a single probe:** a level is in the window if
and only if its price is penny-aligned and its tick is in `[base, base + 4096)`.
Everything else is in the overflow map. `find` tests the range and probes
exactly one of the two, never both.

Keeping that true is the whole job of rebasing, and it has to move levels in
*both* directions: levels that fall out of the new window go to the overflow
map, and levels the window has just slid over come back in. Missing the second
direction would leave a level in the overflow map that `find` no longer looks
for, so lookups would start silently missing. `ladder_pulls_levels_back_in_when
_the_window_returns` exists for exactly that.

Rebasing walks the occupied bits, not the 4096 slots, so it costs O(live levels)
-- at most a few thousand -- and never memmoves the array.

### Why the overflow map is ordered

`std::map`, not a hash map, and not because of lookup speed. When the entire
book is outside the window, `best()` still has to be correct, and an unordered
container cannot answer "highest price present". That case is real: in
pre-market the only resting orders can be stub quotes, and the window is
somewhere else entirely.

The overflow map is cold and tiny by construction -- the measurement says it
carries 0.1% to 1.1% of insertions plus the stub quotes -- so the node-per-entry
cost it brings is paid on traffic that was never going to be fast anyway.

### Two bugs the tests found

**The window could be stranded by a sub-penny inside.** `maybe_rebase` bailed
out when the best price was not penny-aligned, on the reasoning that the window
cannot store such a price anyway. But the anchor and the storage are different
questions: a sub-penny inside still says where the action is, and the aligned
prices around it are exactly what the window should cover. With the bail-out,
a ladder whose first resting price was sub-penny never rebased at all and every
subsequent aligned insert went to the overflow map. The anchor is now
`tick_of(best())` regardless of alignment.

**`best()` was `noexcept` and contained an assertion.** In the test build the
assert handler throws, so that combination calls `std::terminate` and the misuse
test crashed the binary instead of passing. The rule now is: a function
containing an assertion is not marked `noexcept`, and
`tools/check_noexcept_asserts.py` enforces it in CI. Nowhere in this codebase
does the annotation buy anything on such a function.

### One test that only failed in release

`ladder_misuse_is_detected` originally expected a duplicate-price insert to
assert. It does -- at invariant level 1 and above, because detecting a duplicate
costs a full `find()` on the insert path, which is the hot path. Level 0 is
where the benchmarks run and where that check is meant to be absent, so it is
`ITCH_INVARIANT` and not `ITCH_ASSERT`.

The test now says so, and skips at level 0. This is the three-level assertion
scheme from section 2 doing its job: the failure was the test asserting a cost
the design deliberately does not pay in release.

## 18. FlatBook: what the layout actually buys

The pieces from sections 15 to 17 assembled. The shape in one sentence: an order
reference becomes a 32-bit handle through an open-addressed index, the handle
names a 32-byte record in a pool, and the record carries a handle to its price
level so that **cancel never consults the ladder at all**.

That last point is the design. On the measured feed, adds are 42% of messages
and deletes 39%. A delete that does not clear its level touches exactly two
cache lines: the index entry and the order record. The ladder is only read when
a level is created and only written when one is created or destroyed.

### Cached best, and what "correct shift" costs

`best(side)` is a load from a cached `Price`, not a scan. The cache is
maintained by two rules:

- an add that is better than the current best replaces it;
- a level clearing at a price that is *not* the best changes nothing.

Only when the top level clears does the cache have to be recomputed, and that is
the occupancy bitset's `clz`/`ctz` scan. `best_recomputes()` counts how often
that happens, and `flat_best_shifts_when_the_top_clears` asserts the counter
does **not** move when a level clears behind the top -- otherwise the cache
would be correct by accident, recomputing every time.

`kNoPrice` is zero and no order can rest at zero, so it doubles as the "this
side is empty" marker and the cache needs no separate flag.

### The order index

Open addressing, linear probing, load factor at or below 0.5, backward-shift
deletion so no tombstone accumulates over a day of churn. Entries are 16 bytes:
eight per 128-byte M4 cache line, so a probe that misses usually stays on the
line it started on.

The table is sized and prefaulted at construction and **never resizes during
replay** -- that is an assertion, not a growth path. Rehashing mid-run would
move every entry and put a multi-millisecond spike into exactly the tail this
project reports.

The hash is a template parameter, `Identity` or `Mixed`. NASDAQ order references
are near-sequential, which makes identity hashing genuinely plausible: under
linear probing, sequential keys fill the table in order and probe perfectly. It
is also exactly the kind of thing that is obvious until it is wrong, so both are
built, both are tested, and the choice will be made by the benchmark rather than
by this paragraph.

**Rejected:** `std::unordered_map` -- a node and a pointer chase per lookup, on
the hottest path in the program. Swiss-table SIMD metadata -- at 16-byte entries
and load factor 0.5 plain probing should match it, and it is complexity this
project does not need. That one is a judgement, not a measurement, and is
recorded as such.

### Backward-shift deletion is where the bug would be

Linear probing promises that every live entry is reachable from its ideal slot
without crossing an empty one. Deleting an entry can break that for everything
behind it, and the repair (Knuth's algorithm R) has to move those entries back
across the hole. Get the cyclic interval test wrong and lookups silently start
missing entries that are still in the table -- for some keys, not all.

So `OrderIndex::validate()` re-derives that property for every live entry, and
`test_order_index` builds collision chains by hand under identity hashing
(where keys 0, N, 2N all land on slot 0) including one that wraps past the end
of the table, then deletes from the front and the middle of them.

### Three implementations, one answer

`NaiveBook` recomputes everything from a flat vector and has no incremental
state to corrupt. `MapBook` is the textbook structure. `FlatBook` is the one
that controls its own memory layout and therefore the one with somewhere to
hide. All three are driven with the same 40,000 random operations -- a drifting
mid so the ladder rebases, and a mix of near, far, stub-quote and sub-penny
prices matching the measured shape of the real feed -- and compared including
per-level queue order, so price-time priority is compared and not just depth.

`book_digest` folds both sides, every level in price order and every order in
queue order, into one 64-bit number. If that matches, the books match.

### The differential tests were mutation-tested

Three deliberate bugs, injected one at a time, to check the suite can actually
see them:

| injected bug | caught by |
|---|---|
| queue tail not updated, breaking FIFO | an invariant assertion in `add` |
| best not recomputed when the top clears | the differential comparison, 2 tests |
| cleared level left in the ladder | **the pool's stale-handle detector** |

The third is worth dwelling on. Leaving a freed level in the ladder means the
next lookup at that price returns a handle to a slot that has been recycled.
Without the generation check that is a silent read of whatever now occupies the
slot, and the book would go quietly wrong. With it, the run stops at the exact
operation that did it. That is the entire argument for generation-checked
handles, demonstrated rather than asserted.

## 19. Benchmark methodology, and three things that were wrong first

The numbers are in NUMBERS.md. What follows is how they were arrived at,
including the attempts that produced misleading results before they were fixed.

### What is compared

`AvlBook` is `FlatBook` with **only** the price-to-level structure swapped: same
pools, same intrusive queues, same order index, a hand-written AVL tree over a
node pool instead of the ladder. That isolates one variable and answers "what
did the ladder buy?" -- **1.46x**.

`MapBook` is the textbook implementation and changes the level structure, the
queue representation, the allocator and the order index all at once. That
answers the blunter "what did the whole design buy?" -- **2.29x**.

Both are worth knowing and they are not the same number. Quoting only the second
would be the easy, misleading choice.

The AVL baseline is deliberately strong: its nodes come from a pooled vector
with a free list, not from the allocator, so the comparison is a tree walk
against an array index rather than a tree against `malloc`.

### Mistake 1: measuring implementations in sequence

The first version timed all of `FlatBook`'s rounds, then all of `AvlBook`'s,
then all of `MapBook`'s. FlatBook's throughput came out anywhere between 16.7
and 26.0 million ops/s across invocations -- a 56% spread -- while MapBook's
varied by 3.6%.

That was not noise. This machine's throughput climbs over the first seconds of a
process, and measuring FlatBook first gave it the cold period **every time**. A
systematic bias, always in the same direction, dressed up as variance.

Rounds are now interleaved round-robin, with two full warmup passes of all three
beforehand. Medians across five separate invocations now agree to within 5%.

### Mistake 2: injecting above the slowest saturation point

The first latency run picked the rate as 50% of *FlatBook's* saturation --
9.4 million ops/s. MapBook saturates at 8.7 million, so its open-loop queue grew
without bound and its p50 came out at **2.8 milliseconds**. That figure is
correct and completely useless: it measures the backlog, not the operation.

The rate is now 50% of the *slowest* implementation's measured throughput, which
is why throughput is measured before latency and not alongside it.

### Mistake 3: quoting a tail this machine cannot measure

The benchmark runs the identical pacing loop with no book operation in it and
reports it as a **harness floor** row. That row is what exposed the problem.

Across five identical invocations, FlatBook's p99 ranged from 1,934 to 4,558 ns
and its p99.9 from 9,116 to 27,336 ns. In one run the *empty* loop's p99 was
454 ns, against 70-85 ns in the others.

macOS has no `isolcpus`, no `nohz_full`, and no way to pin a thread to a core.
The benchmark requests `QOS_CLASS_USER_INTERACTIVE`, which is as close as this
machine gets, and it is not close enough.

**So no p99 or p99.9 number from this machine is quoted anywhere.** The p50 is
quoted, because it sits at 73-90 ns against a measured 41 ns timer floor and
varies by under 20%. The tail is Linux box work and until that runs, the number
does not exist. The observed ranges are in NUMBERS.md as evidence for *why*,
labelled as such.

### Other things the harness does

- **Both dead-code defences.** Each timed pass ends by computing the book digest
  through `do_not_optimize`, and the benchmark exits if that digest is zero --
  an impossible value for a book that was actually built.
- **The benchmark is also a correctness test.** All three implementations must
  produce the same digest or it refuses to print a result. A fast wrong answer
  is not a result.
- **It refuses to lie about the build.** If `NDEBUG` is not defined the header
  line says the numbers are meaningless rather than printing them plainly.
- **The timer is measured, not assumed.** `timer_resolution_ns()` reports the
  smallest non-zero gap between consecutive clock reads (41 ns here) and
  `timer_call_cost_ns()` the cost of a read (about 15 ns). Both are printed
  above every result, so a latency near the floor can be read for what it is.
- **The workload is real.** 475,247 actual QQQ operations from the feed, not
  generated traffic. A synthetic workload with a uniform price distribution
  would benchmark a book nobody is running -- the measured feed is 42% adds and
  39% deletes, half of all insertions land exactly on the inside, and every
  symbol carries permanent stub quotes.

## 20. Profiling: two bottlenecks, and one fix that did not pay

Found with two tools used together, because neither alone is sufficient. The
book's own counters say what the structure is *doing* -- probes per index
lookup, how often the best price is recomputed, how much traffic misses the
ladder window -- which points at a cause. `/usr/bin/sample` says where the time
*goes*, which points at a location. A hot function with a good reason to be hot
is not a bottleneck, and a bad counter in cold code is not one either.

`bench/bench_profile` runs the real workload in a loop and prints both.

### What the counters said first

Over one pass of QQQ's 475,247 operations:

| | |
|---|---|
| index probes per operation | **0.0311** |
| best price recomputes | 16,155 (3.40% of operations) |
| ladder window hits / overflow | 249,267 / 2,021 (**0.80% overflow**) |
| ladder rebases | **3** for the whole session |

The 0.80% overflow rate confirms the section 15 prediction (98.9% to 99.9%
in-window) on the actual run, and three rebases in a session means the rebasing
machinery costs nothing at all. Neither is a bottleneck. That is what the
counters are for: ruling things out cheaply.

### Bottleneck 1: the order index was sized four times too large

The index was sized from the *order pool* capacity, on the reasoning that it has
to hold every live order. But the pool is deliberately generous -- it is dense,
prefaulted once, and never grows -- while the index is open-addressed and its
probes land anywhere in it. Sizing one from the other made a 524,288-entry,
8 MiB table holding 6,819 live entries.

A sweep, with the sizes **interleaved** so the machine's warm-up drift could not
land systematically on one of them:

| entries | load factor | ops/s |
|---|---|---|
| 16,384 | 43.2% | 18,836,147 |
| 32,768 | 21.6% | 23,466,717 |
| 65,536 | 10.8% | 25,875,752 |
| **131,072** | **5.4%** | **26,974,316** |
| 262,144 | 2.7% | 24,844,157 |
| 524,288 | 1.3% | 20,698,310 |

There is an optimum and **both sides of it are worse**, which is the part worth
noticing. Below it the table is small but the load factor makes probe chains
long. Above it the chains are short but a lookup is a cache and TLB miss in a
table that is almost entirely empty. The old default sat at the far end.

Index capacity is now a separate constructor parameter from order capacity, and
the default is 131,072 entries. **20.0 -> 26.5 million ops/s, 1.32x.**

The first version of this sweep ran the sizes in ascending order and showed
larger-is-always-better, because the smallest size got the cold start. The same
mistake as section 19, caught the second time by habit rather than by luck.

### Bottleneck 2: handle validation did four branches where two suffice

After the first fix, `Pool<Order>::deref_checked` was **15.6% of the profile** --
the largest entry after `main`. It runs on every handle dereference, and
`reduce` alone does three of them.

It was doing four checks. Two are provably redundant:

- **The null check.** A null handle's index is `kNullIndex`, which is defined
  equal to `kMaxCapacity`, and a pool's capacity is asserted at construction to
  be at most `kMaxCapacity`. So a null handle's index is never less than
  `slots_.size()` and the bounds check already rejects it.
- **The liveness check.** `allocate()` only ever issues a handle whose
  generation it has just incremented to odd. A handle in circulation therefore
  always carries an odd generation, so `slot.generation() == h.generation()`
  already implies the slot's generation is odd, which is what live means.

Both implications are now `static_assert`ed where they can be, and neither
weakens detection: the null, out-of-range, use-after-free, double-free and
slot-reuse tests are unchanged and all 17 still pass.

**26.8 -> 30.0 million ops/s, 1.11x**, reproduced across three separate process
invocations.

### The fix that did not pay, recorded because it did not

The profile showed `deref_checked` as an out-of-line symbol, suggesting it had
been pushed past the inliner's threshold by the four assertion call sites --
each of which materialised a five-field `AssertInfo` temporary. Passing the
arguments individually to a `cold, noinline` function should have shrunk the
call sites enough to let it inline.

One measurement showed 26.5 -> 27.7 million ops/s, a 4.5% win. Three more runs
of each version gave 26.8, 27.3, 27.1 against 27.2 to 27.7 -- overlapping
ranges -- and the count of out-of-line `deref_checked` symbols in the binary was
identical either way.

**It was noise.** The change was reverted rather than kept with a number
attached to it. A 4.5% claim needs more than one run, and the first run is
exactly when a plausible number is most tempting.

### Result

| | before | after | |
|---|---|---|---|
| FlatBook | 20,010,189 ops/s | **28,803,939 ops/s** | **1.44x** |
| vs AvlBook | 1.46x | 1.92x | |
| vs MapBook | 2.29x | 3.47x | |

Both fixes were sizing and branch-count changes to code that was already
correct. Neither changed a data structure, and neither was guessable -- the
index sweep in particular found an optimum whose shape contradicted the
hypothesis that sent me looking.

## 21. The order record: a claim of mine that measurement killed

`Order` is 32 bytes, and section 18 justified that two ways: a power of two so
indexing is a shift rather than a multiply, and four per 128-byte cache line.

Both were measurable, so both were measured. A compile-time knob
(`ITCH_ORDER_PAD_BYTES`) appends unused bytes to `Order`, changing its size and
nothing else, and the same workload runs against each.

| sizeof(Order) | per 128 B line | power of two | ops/s |
|---|---|---|---|
| **32** | 4 | yes | **59,447,200** |
| 40 | 3 | no | 56,587,736 |
| 56 | 2 | no | 57,262,185 |
| **64** | 2 | yes | **59,041,960** |

**The power-of-two claim holds.** 32 and 64 both beat 40 and 56 by 2-4%, and the
pattern is identical under both hash functions, so it is the indexing arithmetic
and not a cache effect.

**The cache-line claim does not.** 64 bytes, at two orders per line instead of
four, is as fast as 32. The reason is obvious once the measurement forces you to
look: every access to an order is a random lookup through a handle. Nothing ever
walks adjacent orders. A second order sharing a cache line is an order that will
never be read on that fetch, so packing four of them in buys nothing.

`Order` stays at 32 bytes -- it is a power of two and smaller is not worse -- but
for one reason rather than two, and the comment in `records.hpp` now says so.

### The 24-byte variant, and why it is not reachable

Dropping the stored `OrderRef` would give 24 bytes. It is load-bearing in two
places: `reduce()` needs it to erase from the order index, and `book_digest()`
needs it to enumerate a level's queue in a form that is comparable across
implementations (handle indices are allocation-order dependent and are not).
Either replacement -- passing the ref down every path, or a parallel array --
gives back what the 8 bytes saved. Recorded as attempted, not as untried.

## 22. The order reference hash: the obvious answer was wrong twice

NASDAQ order references are near-sequential, which made identity hashing
plausible under linear probing. Both were built behind the same template
parameter from the start, precisely so this could be settled by measurement.
It took three measurements to get to the right answer, and the first two both
pointed the wrong way.

### Measurement 1: identity does far more probes and is still faster

| symbol | peak live | splitmix64 probes/op | identity probes/op | identity throughput |
|---|---|---|---|---|
| SPY | 2,985 | 0.052 | 0.195 | -- |
| QQQ | 7,679 | 0.162 | 0.596 | **+6.91%** |
| AMD | 15,286 | 0.382 | 1.282 | **+3.22%** |
| AAPL | 42,774 | 1.510 | 4.323 | **+7.88%** |

Identity does three to four times more probes on every symbol and wins on every
symbol. Near-sequential keys land in adjacent slots, so a probe chain walks
forward inside a cache line already paid for, while splitmix64's shorter chains
are random and each step is a likely miss. Counting probes measures the wrong
thing; what costs is where they land.

On that evidence identity became the default.

### Measurement 2: at the right table size it stops mattering

Sweeping the index size on AAPL, the densest symbol measured:

| entries | load factor | identity | splitmix64 |
|---|---|---|---|
| 131,072 | 32.6% | 40,203,013 | 37,168,362 |
| 262,144 | 16.3% | 44,678,252 | 44,490,716 |
| **524,288** | **8.2%** | **47,897,214** | **47,993,190** |
| 1,048,576 | 4.1% | 45,500,697 | 43,600,107 |

At the optimum the two are within noise, with splitmix64 marginally ahead.
Identity's advantage appears only when the table is mis-sized. So sizing the
table matters more than the hash does, and the hash choice is mostly insurance
against getting the size wrong.

The optimum is a **load factor, not a size**: it appeared at 5.9% on a symbol
peaking at 7,679 live orders and 8.2% on one peaking at 42,774. That is a rule,
so it is now code -- `index_entries_for(peak_live_orders)`.

### Measurement 3: identity has an unbounded delete, and it is disqualifying

This one came from somewhere else entirely. `bench_cancel` exists to test a
different claim -- that cancelling from the middle of a queue is O(1) -- by
cancelling at various depths and positions. Under identity hashing:

| depth | head | 25% | middle | tail |
|---|---|---|---|---|
| 64 | 126.9 | 92.8 | 70.6 | 24.9 |
| 1,024 | 1,220.4 | 626.8 | 383.3 | 14.2 |
| 16,384 | 8,843.2 | 6,733.4 | 4,436.8 | 13.6 |
| 100,000 | **54,878.8** | 40,813.1 | 27,092.0 | 14.7 |

Under splitmix64, same harness, same runs:

| depth | head | 25% | middle | tail |
|---|---|---|---|---|
| 64 | 30.7 | 28.0 | 28.9 | 26.8 |
| 1,024 | 16.1 | 14.4 | 12.8 | 10.8 |
| 16,384 | 21.1 | 25.7 | 21.3 | 11.3 |
| 100,000 | **26.0** | 19.2 | 17.1 | 14.9 |

**The cost is not in the queue at all.** Unlinking an order is a fixed number of
field writes either way. It is the order index: sequential keys under identity
hashing form one unbroken probe cluster, and backward-shift deletion has to walk
that cluster from the erased slot to the next empty one. Erase the front of a
100,000 key cluster and you walk 100,000 entries.

Deletes are **43% of this feed**. A few percent of steady-state throughput is
not worth an unbounded tail on the second most common operation in the system.
**splitmix64 is the default.**

### Why the probe counter did not catch it

`probes_per_op` looked fine for identity -- 0.596 on QQQ. It was blind, because
`erase_at`'s shift loop never incremented the probe counter. The metric measured
insert and lookup and silently ignored the operation with the pathological case.

The index now counts `shifts()` and `longest_shift()` separately, because they
are a different cost with a different worst case and folding them into one
number is exactly how this hid.

The wider lesson, and the reason this section is the longest in the file: two
independent measurements agreed on an answer that a third, looking at a
different question entirely, showed was wrong. Steady-state throughput on real
data said identity. Table sizing said it barely mattered. Only a benchmark
written to test an unrelated claim about queue cancellation exposed a worst case
that a latency-sensitive system cannot accept.

## 23. What a second compiler found

For most of this project only one compiler had ever built it: Apple Clang on
arm64. The CI workflow existed from the first commit and had never actually run,
because there was no remote. Pushing it found four things in three rounds, and
none of them were style.

### The one that was a real gap

`ITCH_ASSERT` did not tell the optimiser anything.

`assert_failed` was not marked `[[noreturn]]`, so a compiler must assume
execution continues past a failed assertion and cannot treat the assertion as a
fact. That became load-bearing when `deref_checked` dropped its explicit null
check on the grounds that the bounds check subsumes it (section 20). Clang
happened to follow that reasoning; GCC did not have to, and rejected
`Pool::allocate` for indexing one past the end.

`[[noreturn]]` is simply correct here -- the default handler aborts, the test
handler throws, and both satisfy it. It was still not enough: GCC's value-range
analysis lost the bound through an inlined `std::vector` allocation. `ITCH_ASSERT`
now emits `__builtin_unreachable()` on the failure path as well.

That hint is attached only to `ITCH_ASSERT`, which is compiled into every
configuration, and the header says why in capitals: **it must never be attached
to a check that can be compiled out.** Asserting a fact nobody verifies converts
a violation from a caught bug into undefined behaviour, which is the opposite of
what the three-level assertion scheme is for.

A second gap fell out of the same investigation. `Pool::allocate` asserted
`!free_head_.is_null()`, which tests the whole 32-bit pattern. A handle carrying
`kNullIndex` with any other generation passes that and still indexes out of
bounds. No such handle can be constructed -- `make()` asserts it -- but that is a
whole-program argument, and asserting the index bound directly is both stronger
and one compare either way.

### The one that was a portability bug in the thing that matters most

The pacer's spin hint was `isb`, which is arm64 only and does not assemble on
x86-64. Every Ubuntu job failed to build.

This is worse than it sounds. `bench_support.hpp` exists for the p99 and p99.9
measurements that this project explicitly defers to a Linux box, and it would
not have compiled there. The gap between "deferred to the Linux box" and "cannot
be built on the Linux box" is the whole plan.

### The two that were the compiler being right about the tests

`test_ladder.cpp` used `std::reverse` without including `<algorithm>`. libc++
supplies it transitively; libstdc++ does not.

Three array-bounds errors all came from tests that pass an invalid handle on
purpose -- `pool_detects_an_out_of_range_handle` hands a handle with index 1000
to a sixteen-slot pool. Constant folded into the call site that is a
compile-time subscript past the end, on a path the assertion exists to stop. The
warning is correct; the test means a bad handle arriving at *runtime*.
`test::opaque()` hides such values from the optimiser, which is also a more
faithful model of the bug being tested.

One genuine false positive remains in that family: GCC's array-bounds analysis
does not model the reallocation in `vector::resize` and reports the zero fill of
the new region as writing past the old end. `WireBuilder::begin` uses `insert`
instead. Changing one call is a smaller price than suppressing a warning class
across the project, and nothing is suppressed anywhere.

### What changed in the workflow

`tools/check_gcc.sh` compiles every translation unit with GCC at `-O3` under the
full warning set. It runs in seconds.

It is not sufficient on its own: local GCC 16 on arm64 did not reproduce the
array-bounds errors that GCC 13 on x86-64 did. Two compilers is better than one,
two versions on two architectures is better than that, and CI is the only place
the second architecture exists. The lesson is narrower than "run CI": a workflow
file that has never executed is not a check, it is an intention.

## 24. Order field ordering, before and after

`Order` carries nine members. In the order they were first written down while
working out what an order needs -- side, then identity, then quantities, then
links -- every 8- and 4-byte member lands after a smaller one and the compiler
realigns it:

```
u8          side;            //  0   1
                             //      7 bytes padding
OrderRef    ref;             //  8   8
u8          gen;             // 16   1
                             //      3 bytes padding
Qty         qty;             // 20   4
u16         owner;           // 24   2
                             //      2 bytes padding
Price       price;           // 28   4
OrderHandle prev, next;      // 32   8
LevelHandle level;           // 40   4
u16         flags_unused;    // 44   2
                             // = 48 bytes, 12 of them padding
```

Sorted widest-first, nothing needs realigning and the small members fill the
tail exactly:

```
OrderRef    ref;    //  0   8
Qty         qty;    //  8   4
Price       price;  // 12   4
OrderHandle prev;   // 16   4
OrderHandle next;   // 20   4
LevelHandle level;  // 24   4
u8          side;   // 28   1
u8          gen;    // 29   1
u16         owner;  // 30   2
                    // = 32 bytes, 0 padding
```

**48 bytes to 32, a third smaller, with no field removed.**

`ITCH_ORDER_NAIVE_LAYOUT` compiles the first form so the difference can be
measured rather than assumed. Same workload, same harness, both layouts:

| layout | sizeof | ops/s |
|---|---|---|
| packed | 32 | 33,395,512 / 33,774,746 |
| naive | 48 | 31,999,192 / 32,419,044 |

**About 4%.** Modest, and consistent with section 21: the win is not density,
because nothing scans adjacent orders. It is that 48 is not a power of two, so
indexing needs a multiply, and that a third less memory means a third fewer
cache lines and TLB entries for the same live set.

Worth stating plainly: a third of the memory for 4% of the time. The memory is
the better half of that trade, and it is the half that does not depend on the
access pattern staying what it is today.

## 25. Matching engine: measured against how much it actually matches

The engine's cost depends on how often an incoming order crosses and how deep it
sweeps. A flow that never crosses measures the add path and calls it matching,
so `bench_engine` parameterises aggression and **reports the fill count next to
every throughput figure**. A number without that is not interpretable.

| flow | orders/s | ns/order | fills | rested |
|---|---|---|---|---|
| 0% aggressive | 10,009,195 | 99.9 | **0** | 400,000 |
| 5% | 9,280,850 | 107.7 | 31,366 | 380,176 |
| 25% | 8,320,109 | 120.2 | 144,699 | 311,610 |
| 60% | 6,730,862 | 148.6 | 294,507 | 233,293 |

Monotonic, and the 0% row is the control: zero fills, every order rests, so it
is the pure add path with the matching check on top.

### The generator had to be built against a live book

"Aggressive" means priced through the opposite touch, and the touch only exists
once there is a book. The first version decided it from a precomputed drifting
mid, and its **0% aggressive flow produced 289,108 fills** -- a bid placed when
the mid was high crosses an offer placed after it fell. All four rows produced
roughly the same fill count and the parameter controlled nothing.

The flow is now built in an untimed first pass against a real book, taking each
price from the actual touch at submission, and the timed pass replays the
recorded prices. Same matching, no generator work inside the loop.

This is the third time a drifting mid has produced a plausible wrong answer in
this project -- the others were the synthetic determinism session and the ladder
benchmark's far prices. A mid that moves independently of the book is not a
market, and anything defined relative to it is defined relative to nothing.

### And the dead-code guard fired on a correct case

`run_once` accumulated filled quantity and exited if it stayed zero. The 0%
aggressive flow trades nothing **by construction**, so the guard rejected a
legitimate configuration. It now accumulates filled plus resting quantity: every
order does one or the other, so it is non-zero for any flow that did work.

A guard against measuring nothing has to be satisfiable by every configuration
that legitimately does something, or it is just an assumption with an exit code.

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
- ~~**Order record width**~~ -- settled by measurement, section 21. The
  power-of-two claim held and the cache-line claim did not; 24 bytes is not
  reachable because the stored reference is load-bearing.
- ~~**Order reference hash**~~ -- settled by measurement, section 22. Identity
  is the default, but the real finding is that sizing the table matters more
  than the hash does.
