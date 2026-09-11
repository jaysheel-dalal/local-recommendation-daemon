#!/usr/bin/env bash
# Investigating per-request latency: compare debug vs release, and vary the
# request count to separate fixed startup cost from per-request cost.
set -u
cd "$(dirname "$0")/.."
SOCK="/tmp/lrd_probe_$$.sock"
cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

for preset in debug release; do
  BIN="build/$preset/bin"
  "$BIN/lrdd" --socket "$SOCK" >/dev/null 2>&1 &
  DAEMON_PID=$!
  for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done
  echo "=== $preset ==="
  for n in 100 1000 5000; do
    printf "  n=%-6s " "$n"
    "$BIN/lrd_cli" --socket "$SOCK" pipeline "$n" 2>&1 | grep -o 'in [0-9]* us ([0-9.]* us per request)'
  done
  kill "$DAEMON_PID" 2>/dev/null; wait "$DAEMON_PID" 2>/dev/null
  rm -f "$SOCK"
done
