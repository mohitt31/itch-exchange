#!/usr/bin/env bash
# The measurements this project defers to a Linux box, in one command.
#
# Two things macOS cannot give:
#
#   perf counters   There is no perf on macOS and no Instruments without a full
#                   Xcode. The 2.11x and 3.81x ratios in NUMBERS.md are measured;
#                   the mechanism behind them is currently reasoned rather than
#                   counted, and that is the one gap between what this project
#                   claims and what it has shown.
#
#   a quiet core    macOS has no isolcpus, no nohz_full and no way to pin a
#                   thread. FlatBook's p99 came out as 202 ns, 37,085 ns, 168 ns
#                   and 5,500,354 ns across four consecutive runs of the same
#                   binary on the same data, while throughput varied by 1%. No
#                   p99 or p99.9 is quoted from that machine.
#
# Usage:
#   tools/linux_counters.sh <corpus.gz> [symbol] [cpu]
#
# For the tail numbers to mean anything the kernel wants isolcpus and nohz_full
# on the chosen CPU, e.g. in the boot line:
#   isolcpus=7 nohz_full=7 rcu_nocbs=7
# The script reports whether that is actually in effect rather than assuming it.
set -uo pipefail
cd "$(dirname "$0")/.."

CORPUS=${1:-}
SYMBOL=${2:-QQQ}
CPU=${3:-}

if [ -z "$CORPUS" ] || [ ! -f "$CORPUS" ]; then
  echo "usage: tools/linux_counters.sh <corpus.gz> [symbol] [cpu]" >&2
  exit 2
fi
if [ "$(uname -s)" != "Linux" ]; then
  echo "error: this is the Linux-only half. On macOS run bench/reproduce.sh." >&2
  exit 2
fi

mkdir -p measurements

echo "=============================================================="
uname -srm
grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/^/cpu      /'
echo "corpus   $CORPUS"
echo "sha256   $(sha256sum "$CORPUS" | awk '{print $1}')"
echo "symbol   $SYMBOL"
echo "compiler $(c++ --version | head -1)"
echo
echo "isolation, as the kernel actually has it:"
printf '  isolcpus   %s\n' "$(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo '(none)')"
printf '  nohz_full  %s\n' "$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || echo '(none)')"
printf '  governor   %s\n' "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'unknown')"
printf '  turbo      %s\n' "$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo 'n/a')"
if [ -z "$(cat /sys/devices/system/cpu/isolated 2>/dev/null)" ]; then
  echo
  echo "  WARNING: no isolated CPU. The tail numbers below will be as"
  echo "  unreliable as the macOS ones, and should not be quoted."
fi
echo "=============================================================="

PIN=""
if [ -n "$CPU" ] && command -v taskset >/dev/null 2>&1; then
  PIN="taskset -c $CPU"
  echo "pinning to cpu $CPU"
fi

echo
echo "### building release"
cmake --preset release >/dev/null && cmake --build --preset release >/dev/null
echo done

# perf needs kernel.perf_event_paranoid <= 1 for most of these.
PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 3)
if [ "$PARANOID" -gt 1 ]; then
  echo
  echo "note: kernel.perf_event_paranoid is $PARANOID; counters need <= 1."
  echo "      sudo sysctl kernel.perf_event_paranoid=1"
fi

EVENTS=cycles,instructions,cache-references,cache-misses,branches,branch-misses,L1-dcache-load-misses,dTLB-load-misses

echo
echo "### 1. counters per book implementation"
echo "this is the number that turns 2.11x and 3.81x from measured into explained"
for impl in flat avl map; do
  echo
  echo "--- $impl ---"
  $PIN perf stat -e "$EVENTS" -- \
    ./build/release/bench/bench_book --only "$impl" --symbol "$SYMBOL" --runs 5 "$CORPUS" \
    2>&1 | tee "measurements/linux_perf_${impl}.txt" | grep -E "ops/s|cycles|instructions|cache-|branch|dTLB|elapsed"
done

echo
echo "### 2. latency tail, on an isolated core"
$PIN ./build/release/bench/bench_book --symbol "$SYMBOL" --runs 7 "$CORPUS" \
  | tee measurements/linux_bench_book.txt | tail -14

echo
echo "### 3. the rest of the suite, for comparison against the M4 figures"
$PIN ./build/release/bench/bench_parse  --runs 5 "$CORPUS" | tee measurements/linux_parse.txt  | tail -4
$PIN ./build/release/bench/bench_pool   --runs 5           | tee measurements/linux_pool.txt   | tail -8
$PIN ./build/release/bench/bench_ladder --runs 5           | tee measurements/linux_ladder.txt | tail -8
$PIN ./build/release/bench/bench_cancel --reps 300         | tee measurements/linux_cancel.txt | tail -11
$PIN ./build/release/bench/bench_engine --runs 5 --orders 400000 | tee measurements/linux_engine.txt | tail -7

echo
echo "### 4. replay determinism, against the macOS digest"
$PIN ./build/release/apps/itch_replay --symbol "$SYMBOL" --runs 10 "$CORPUS" \
  | tee measurements/linux_replay.txt | tail -4
echo
echo "the digest above must match the one in NUMBERS.md. A different answer on a"
echo "different architecture would mean the replay depends on something it should"
echo "not."

echo
echo "written to measurements/linux_*.txt"
