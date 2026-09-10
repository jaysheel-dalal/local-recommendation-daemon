#!/usr/bin/env bash
# Focused check on the one end-to-end claim that matters: at high client
# concurrency, does shard count actually change throughput? 5 trials each,
# fresh daemon per configuration, interleaved so drift in machine load hits
# both arms equally.
set -u
cd "$(dirname "$0")/.."
BIN=build/release/bin
SOCK="/tmp/lrd_conf_$$.sock"
cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

run_arm() {
  local shards=$1
  "$BIN/lrdd" --socket "$SOCK" --threads 16 --capacity 20000 --shards "$shards" >/dev/null 2>&1 &
  DAEMON_PID=$!
  for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done
  printf "shards=%-3s " "$shards"
  "$BIN/lrd_bench" --socket "$SOCK" --threads 16 --requests 5000 --warmup 1000 \
      --trials 5 2>/dev/null | grep -E '^ +16 |ops/sec across' | tr '\n' ' '
  echo
  kill "$DAEMON_PID" 2>/dev/null; wait "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"
}

for round in 1 2; do
  echo "--- round $round ---"
  run_arm 1
  run_arm 16
done
