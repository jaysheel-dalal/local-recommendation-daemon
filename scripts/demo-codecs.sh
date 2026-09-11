#!/usr/bin/env bash
# Step 8 demo: the two codecs end to end, and what happens when they disagree.
set -u
cd "$(dirname "$0")/.."
BIN=build/release/bin
SOCK="/tmp/lrd_codec_$$.sock"
cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

start_daemon() {
  "$BIN/lrdd" --socket "$SOCK" --codec "$1" --threads 16 --capacity 20000 >/dev/null 2>&1 &
  DAEMON_PID=$!
  for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done
}
stop_daemon() { kill "$DAEMON_PID" 2>/dev/null; wait "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }

for codec in binary protobuf; do
  echo "=============================================================="
  echo " daemon and client both speaking $codec"
  echo "=============================================================="
  start_daemon "$codec"
  "$BIN/lrd_cli" --socket "$SOCK" --codec "$codec" put greeting "hello over $codec" 2>&1 | tail -1
  echo -n "  get -> "; "$BIN/lrd_cli" --socket "$SOCK" --codec "$codec" get greeting 2>/dev/null
  "$BIN/lrd_bench" --socket "$SOCK" --codec "$codec" --threads 8 --requests 5000 \
      --warmup 1000 --trials 3 2>/dev/null | tail -3
  stop_daemon
  echo
done

echo "=============================================================="
echo " mismatch: protobuf daemon, binary client"
echo "=============================================================="
start_daemon protobuf
"$BIN/lrd_cli" --socket "$SOCK" --codec binary get greeting 2>&1 | tail -2
echo "  client exit=$?"
stop_daemon
