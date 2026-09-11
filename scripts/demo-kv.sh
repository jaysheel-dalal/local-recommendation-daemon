#!/usr/bin/env bash
# Step 2 milestone demo: the framed GET/PUT/DELETE/STATS protocol end to end
# over a real UNIX domain socket.
set -u
cd "$(dirname "$0")/.."
BIN="build/${1:-debug}/bin"
SOCK="/tmp/lrd_demo_$$.sock"

cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

cli() { timeout 30 "$BIN/lrd_cli" --socket "$SOCK" "$@"; }

echo "### starting daemon"
"$BIN/lrdd" --socket "$SOCK" &
DAEMON_PID=$!
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

echo
echo "### put / get"
cli put greeting "hello from the framed protocol"
cli get greeting

echo
echo "### get a key that does not exist"
cli get nope; echo "exit=$?"

echo
echo "### delete, then get, then delete again"
cli del greeting
cli get greeting; echo "exit=$?"
cli del greeting; echo "exit=$?"

echo
echo "### a 400 KB value (frame spans many reads and writes)"
cli put big --size 400000
cli get big | wc -c

echo
echo "### a value over the 512 KB cap is refused client-side"
cli put toobig --size 600000; echo "exit=$?"

echo
echo "### 2000 put/get pairs down one connection"
cli pipeline 1000

echo
echo "### daemon counters"
cli stats
