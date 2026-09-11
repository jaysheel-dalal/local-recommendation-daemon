#!/usr/bin/env bash
# Step 11 demo: the example application talking to the daemon through the SDK.
set -u
cd "$(dirname "$0")/.."
BIN="build/${1:-release}/bin"
SOCK="/tmp/lrd_sdk_demo_$$.sock"
cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

echo "### daemon with an exposure cap of 2 per item"
"$BIN/lrdd" --socket "$SOCK" --capacity 500 --shards 8 --exposure-cap 2 >/dev/null 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

echo
echo "### running examples/recommender_app.cpp -- linked only against lrd_sdk"
"$BIN/lrd_example_app" "$SOCK"

echo
echo "### running it again: the caps from the first run still apply"
"$BIN/lrd_example_app" "$SOCK" | tail -12
