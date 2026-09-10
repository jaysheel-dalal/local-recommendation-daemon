#!/usr/bin/env bash
# Step 1 milestone demo: an echo round-trip over a real UNIX domain socket,
# including a payload big enough to force partial reads, and a demonstration of
# stale-socket recovery.
set -u
cd "$(dirname "$0")/.."
BIN="build/${1:-debug}/bin"
SOCK="/tmp/lrd_demo_$$.sock"

cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

echo "### starting daemon"
"$BIN/lrdd" --socket "$SOCK" &
DAEMON_PID=$!
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

echo
echo "### socket file permissions"
ls -l "$SOCK"

echo
echo "### small payload"
"$BIN/lrd_cli" --socket "$SOCK" --message "hello from lrd_cli"

echo
echo "### 18 MB payload (forces partial reads and writes)"
timeout 60 "$BIN/lrd_cli" --socket "$SOCK" --message "0123456789abcdefgh" --repeat 1000000 || echo "TIMED OUT OR FAILED (exit=$?)"

echo
echo "### connecting when nothing is listening"
"$BIN/lrd_cli" --socket /tmp/lrd_definitely_not_here.sock --message hi
echo "exit=$?"

echo
echo "### SIGKILL the daemon, leaving a stale socket file behind"
kill -9 "$DAEMON_PID" 2>/dev/null
wait "$DAEMON_PID" 2>/dev/null
ls -l "$SOCK" 2>/dev/null && echo "(socket file survived, as expected)"

echo
echo "### restarting: bind() must detect the stale file and reclaim it"
"$BIN/lrdd" --socket "$SOCK" &
DAEMON_PID=$!
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done
"$BIN/lrd_cli" --socket "$SOCK" --message "recovered"
