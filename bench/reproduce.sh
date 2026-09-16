#!/usr/bin/env bash
# Regenerates every number in the README, in one command.
#
#   bench/reproduce.sh [--corpus PATH] [--symbol SYM] [--rounds N]
#
# With no corpus it looks for one in data/, and fetches a bounded prefix if
# there is none. Everything it produces lands in measurements/, and every file
# records the input it came from.
set -euo pipefail
cd "$(dirname "$0")/.."

CORPUS=""
SYMBOL=QQQ
ROUNDS=9
PREFIX_BYTES=1G

while [ $# -gt 0 ]; do
  case "$1" in
    --corpus) CORPUS="$2"; shift 2 ;;
    --symbol) SYMBOL="$2"; shift 2 ;;
    --rounds) ROUNDS="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ -z "$CORPUS" ]; then
  CORPUS=$(ls -S data/*.gz 2>/dev/null | head -1 || true)
fi
if [ -z "$CORPUS" ] || [ ! -f "$CORPUS" ]; then
  echo "no corpus in data/; fetching a $PREFIX_BYTES prefix"
  tools/fetch_data.sh --bytes "$PREFIX_BYTES"
  CORPUS=$(ls -S data/*.gz | head -1)
fi

mkdir -p measurements

echo "=============================================================="
echo "corpus   $CORPUS"
echo "sha256   $(shasum -a 256 "$CORPUS" | awk '{print $1}')"
echo "bytes    $(wc -c < "$CORPUS" | tr -d ' ')"
echo "symbol   $SYMBOL"
echo "machine  $(uname -sr) $(uname -m)"
if command -v sysctl >/dev/null 2>&1; then
  echo "cpu      $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
fi
echo "compiler $(c++ --version | head -1)"
echo "=============================================================="

echo
echo "### building release"
cmake --preset release > /dev/null
cmake --build --preset release > /dev/null
echo "done"

echo
echo "### 1. feed statistics and framing health"
./build/release/apps/itch_stats --by-symbol 20 "$CORPUS" \
  | tee measurements/stats.txt | head -30

echo
echo "### 2. price distribution (decides the ladder geometry)"
./build/release/apps/itch_histogram --symbol "$SYMBOL" \
  --json "measurements/histogram_${SYMBOL}.json" "$CORPUS" \
  | tee "measurements/histogram_${SYMBOL}.txt"

echo
echo "### 3. three-way book benchmark, each implementation alone in its own process"
rm -f "measurements/bench_book_${SYMBOL}.txt"
for impl in flat avl map; do
  ./build/release/bench/bench_book --only "$impl" --symbol "$SYMBOL" --runs 5 "$CORPUS" \
    | tee -a "measurements/bench_book_${SYMBOL}.txt"
done

echo
echo "### 3b. interleaved, for the cross-implementation digest check"
./build/release/bench/bench_book --symbol "$SYMBOL" --runs "$ROUNDS" "$CORPUS" \
  | tee "measurements/bench_book_interleaved_${SYMBOL}.txt"

echo
echo "### 4. parser, no decompressor in the loop"
./build/release/bench/bench_parse --runs 5 "$CORPUS" | tee measurements/bench_parse.txt

echo
echo "### 5. order pool, and the prefault check"
./build/release/bench/bench_pool --runs 5 | tee measurements/bench_pool.txt

echo
echo "### 6. ladder: window path vs overflow path"
./build/release/bench/bench_ladder --runs 5 | tee measurements/bench_ladder.txt

echo
echo "### 7. cancel from the middle of a queue is O(1)"
./build/release/bench/bench_cancel --reps 300 | tee measurements/bench_cancel.txt

echo
echo "### 7b. matching engine, by how aggressive the flow is"
./build/release/bench/bench_engine --runs 5 --orders 400000 | tee measurements/bench_engine.txt

echo
echo "### 8. deterministic replay, ten runs"
./build/release/apps/itch_replay --symbol "$SYMBOL" --runs 10 --validate-every 100000 \
  "$CORPUS" | tee "measurements/replay_${SYMBOL}.txt"

echo
echo "### 9. tests, all four configurations"
for p in release release-checked asan-ubsan tsan; do
  cmake --preset "$p" > /dev/null
  cmake --build --preset "$p" > /dev/null
  printf '%-16s ' "$p"
  ctest --preset "$p" 2>&1 | grep -E 'tests passed|tests failed'
done

echo
echo "everything written to measurements/"
