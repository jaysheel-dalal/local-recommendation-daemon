#!/usr/bin/env bash
# Runs both benchmarks and prints the numbers the README quotes.
#
# Always the release preset: an -O0 build measures the compiler's inlining
# decisions more than the code's design.
set -u
cd "$(dirname "$0")/.."
BIN=build/release/bin
SOCK="/tmp/lrd_bench_$$.sock"

if [ ! -x "$BIN/lrd_bench" ]; then
  echo "release build missing; run: scripts/build.sh release" >&2
  exit 1
fi

cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

echo "=============================================================="
echo " Baseline: what one IPC round trip costs on this machine"
echo "=============================================================="
"$BIN/lrd_baseline_pingpong" 20000
"$BIN/lrd_baseline_framing_shape" 20000 single
"$BIN/lrd_baseline_framing_shape" 20000 split

echo
echo "=============================================================="
echo " End to end: clients -> socket -> daemon -> locked cache"
echo "=============================================================="
# 16 worker threads so that even the 16-client sweep point has a worker each.
# With fewer, the surplus connections would sit unserved and the numbers would
# be measuring the starvation documented on Server, not the daemon's speed.
"$BIN/lrdd" --socket "$SOCK" --threads 16 --capacity 20000 >/dev/null 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

"$BIN/lrd_bench" --socket "$SOCK" --sweep --requests "${1:-8000}" --warmup 1000 --trials "${3:-3}"

echo
echo "=============================================================="
echo " In process: the same cache with no sockets in the way"
echo "=============================================================="
"$BIN/lrd_cache_bench" --sweep --ops "${2:-300000}" --trials "${3:-3}"
