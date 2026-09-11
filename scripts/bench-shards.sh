#!/usr/bin/env bash
# Step 7: does sharding help, and where does it help?
#
# Three measurements:
#   1. Shard sweep in-process   - the effect sharding is meant to have.
#   2. Thread sweep, 1 vs N shards - the step 6 curve, repaired or not.
#   3. End to end               - whether any of it reaches a client.
set -u
cd "$(dirname "$0")/.."
BIN=build/release/bin
OPS="${1:-300000}"
TRIALS="${2:-3}"
SOCK="/tmp/lrd_shard_$$.sock"

cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

echo "=============================================================="
echo " 1. In process, 8 threads, sweeping shard count"
echo "=============================================================="
"$BIN/lrd_cache_bench" --shard-sweep --threads 8 --ops "$OPS" --trials "$TRIALS"

echo
echo "=============================================================="
echo " 2. In process, sweeping threads: 1 shard vs 16 shards"
echo "=============================================================="
echo "--- 1 shard (the step 5 design) ---"
"$BIN/lrd_cache_bench" --sweep --shards 1 --ops "$OPS" --trials "$TRIALS" | tail -6
echo "--- 16 shards ---"
"$BIN/lrd_cache_bench" --sweep --shards 16 --ops "$OPS" --trials "$TRIALS" | tail -6

echo
echo "=============================================================="
echo " 3. End to end: does any of this reach a client?"
echo "=============================================================="
for shards in 1 16; do
  echo "--- daemon with $shards shard(s) ---"
  "$BIN/lrdd" --socket "$SOCK" --threads 16 --capacity 20000 --shards "$shards" \
      >/dev/null 2>&1 &
  DAEMON_PID=$!
  for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done
  "$BIN/lrd_bench" --socket "$SOCK" --sweep --requests 5000 --warmup 1000 \
      --trials "$TRIALS" 2>/dev/null | tail -12
  kill "$DAEMON_PID" 2>/dev/null
  wait "$DAEMON_PID" 2>/dev/null
  rm -f "$SOCK"
done
