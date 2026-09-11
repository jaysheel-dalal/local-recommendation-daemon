#!/usr/bin/env bash
# Is the end-to-end codec gap a latency effect or a CPU-saturation effect?
#
# Hypothesis: at one client thread the system is latency-bound (the thread is
# blocked in the kernel most of the time), so codec CPU is invisible. At high
# concurrency on an 8-core box every core is busy and the system becomes
# CPU-bound, so codec CPU converts directly into lost throughput.
#
# If true: the two codecs should be near-identical at 1 thread and diverge as
# threads increase.
set -u
cd "$(dirname "$0")/.."
BIN=build/release/bin
SOCK="/tmp/lrd_ce2e_$$.sock"
cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

for codec in binary protobuf; do
  echo "=== $codec ==="
  "$BIN/lrdd" --socket "$SOCK" --codec "$codec" --threads 16 --capacity 20000 >/dev/null 2>&1 &
  DAEMON_PID=$!
  for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done
  for t in 1 2 8; do
    printf "  threads=%-3s " "$t"
    "$BIN/lrd_bench" --socket "$SOCK" --codec "$codec" --threads "$t" --requests 5000 \
        --warmup 1000 --trials 3 2>/dev/null | grep -E "^ +$t " | awk '{printf "%10s ops/sec   p50=%s us\n", $2, $4}'
  done
  kill "$DAEMON_PID" 2>/dev/null; wait "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"
done

echo
echo "--- cores available ---"
nproc
