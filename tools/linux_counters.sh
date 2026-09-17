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

EXPECTED_SHA=8c97b5b13bc451c012c2466fb7e258da134dab29aa47b67fe7b0088c78e870be
EXPECTED_REPLAY_DIGEST=6344a2790a894bc5
EXPECTED_BOOK_DIGEST=a0ed37f623c3aa3f

missing=()
for tool in cmake ninja c++ taskset sha256sum lscpu; do
  command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if [ ${#missing[@]} -ne 0 ]; then
  echo "missing tools: ${missing[*]}" >&2
  echo "on Ubuntu:" >&2
  echo "  sudo apt install -y build-essential cmake ninja-build zlib1g-dev util-linux" >&2
  exit 2
fi

echo "=============================================================="
uname -srm
grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/^/cpu      /'
echo "corpus   $CORPUS"
GOT_SHA=$(sha256sum "$CORPUS" | awk '{print $1}')
echo "sha256   $GOT_SHA"
if [ "$GOT_SHA" != "$EXPECTED_SHA" ]; then
  echo "error: corpus hash does not match NUMBERS.md ($EXPECTED_SHA)." >&2
  echo "       Every figure this script produces would be for a different input." >&2
  exit 1
fi
echo "         matches NUMBERS.md"
echo "symbol   $SYMBOL"
echo "compiler $(c++ --version | head -1)"
echo
echo "isolation, as the kernel actually has it:"
printf '  isolcpus   %s\n' "$(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo '(none)')"
printf '  nohz_full  %s\n' "$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || echo '(none)')"
printf '  governor   %s\n' "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'unknown')"
printf '  turbo      %s\n' "$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo 'n/a')"
printf '  on AC      %s\n' "$(cat /sys/class/power_supply/A*/online 2>/dev/null | head -1 || echo 'unknown')"
echo
echo "cpu topology (hybrid Intel parts have P-cores with higher MAXMHZ than E-cores):"
lscpu -e=CPU,CORE,MAXMHZ 2>/dev/null | sed 's/^/  /'
if [ -n "$CPU" ]; then
  core_max=$(lscpu -e=CPU,MAXMHZ 2>/dev/null | awk -v c="$CPU" '$1==c{print $2}')
  top_max=$(lscpu -e=CPU,MAXMHZ 2>/dev/null | awk 'NR>1{print $2}' | sort -n | tail -1)
  if [ -n "$core_max" ] && [ "$core_max" != "$top_max" ]; then
    echo
    echo "  WARNING: cpu $CPU has MAXMHZ $core_max, below the fastest core's $top_max."
    echo "  On a hybrid part that is an E-core, and every number would be for the"
    echo "  wrong core. Pick a CPU with the highest MAXMHZ."
  fi
fi
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

# The counters are opened by bench_book itself with exclude_kernel set, which
# needs kernel.perf_event_paranoid <= 2. Ubuntu ships 4.
PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 4)
if [ "$PARANOID" -gt 2 ]; then
  echo
  echo "kernel.perf_event_paranoid is $PARANOID; counters need <= 2. Setting it for"
  echo "this boot only:"
  sudo sysctl -w kernel.perf_event_paranoid=2
fi

echo
echo "### 1. counters per book implementation, timed region only"
echo "this is what turns 2.11x and 3.81x from measured into explained."
echo "NOT perf stat around the process: building the workload inflates 4.7 GB and"
echo "dispatches 368M messages first, so process-wide counters would be ~99% gzip."
for impl in flat avl map; do
  echo
  echo "--- $impl ---"
  $PIN ./build/release/bench/bench_book --only "$impl" --counters --symbol "$SYMBOL" \
    --runs 7 "$CORPUS" | tee "measurements/linux_counters_${impl}.txt" \
    | grep -E "median|^FlatBook|^AvlBook|^MapBook|/op|IPC|multiplexed|unavailable"
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
GOT_DIGEST=$(awk '/^digest/{print $2}' measurements/linux_replay.txt)
if [ "$GOT_DIGEST" = "$EXPECTED_REPLAY_DIGEST" ]; then
  echo "replay digest matches the macOS arm64 run: $GOT_DIGEST"
else
  echo "REPLAY DIGEST MISMATCH: got $GOT_DIGEST, macOS arm64 gave $EXPECTED_REPLAY_DIGEST"
  echo "replay depends on something it should not. That is a bug, not a measurement."
fi
echo
echo "the digest above must match the one in NUMBERS.md. A different answer on a"
echo "different architecture would mean the replay depends on something it should"
echo "not."

echo
echo "written to measurements/linux_*.txt"
