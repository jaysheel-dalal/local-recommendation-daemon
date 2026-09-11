#!/usr/bin/env bash
# Demonstrates the coordinated-omission correction: the same daemon measured
# closed-loop and then open-loop at a fixed arrival rate.
set -u
cd "$(dirname "$0")/.."
BIN=build/release/bin
SOCK="/tmp/lrd_ol_$$.sock"
cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

"$BIN/lrdd" --socket "$SOCK" --threads 8 --capacity 20000 >/dev/null 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

echo "--- closed loop, 8 threads (as fast as replies arrive) ---"
"$BIN/lrd_bench" --socket "$SOCK" --threads 8 --requests 4000 --warmup 500 --trials 1 2>/dev/null \
  | tail -2

echo
echo "--- open loop, 8 threads at 2000 req/sec/thread ---"
"$BIN/lrd_bench" --socket "$SOCK" --threads 8 --requests 4000 --warmup 500 --trials 1 \
  --rate 2000 2>/dev/null | tail -2
