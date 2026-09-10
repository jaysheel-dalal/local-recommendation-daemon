#!/usr/bin/env bash
# Repeats the end-to-end sweep N times so run-to-run variance is visible.
# A single run on a noisy virtualised host is an anecdote, not a measurement.
set -u
cd "$(dirname "$0")/.."
BIN=build/release/bin
SOCK="/tmp/lrd_rep_$$.sock"
runs="${1:-3}"

cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

"$BIN/lrdd" --socket "$SOCK" --threads 16 --capacity 20000 >/dev/null 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

for r in $(seq 1 "$runs"); do
  echo "--- run $r ---"
  "$BIN/lrd_bench" --socket "$SOCK" --sweep --requests 6000 --warmup 1000 2>/dev/null \
    | grep -E '^ +[0-9]+ '
done
