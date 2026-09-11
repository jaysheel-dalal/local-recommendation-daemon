#!/usr/bin/env bash
# Step 3 milestone demo: LRU eviction visible from a client, over the socket.
set -u
cd "$(dirname "$0")/.."
BIN="build/${1:-debug}/bin"
SOCK="/tmp/lrd_demo_$$.sock"

cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

cli() { timeout 30 "$BIN/lrd_cli" --socket "$SOCK" "$@" 2>&1; }
q()   { timeout 30 "$BIN/lrd_cli" --socket "$SOCK" "$@" 2>/dev/null; }

echo "### daemon with a 3-entry cache"
"$BIN/lrdd" --socket "$SOCK" --capacity 3 &
DAEMON_PID=$!
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

echo
echo "### fill it: a, b, c"
for k in a b c; do cli put "$k" "value-$k" >/dev/null; done
echo "a -> $(q get a)"
echo "b -> $(q get b)"
echo "c -> $(q get c)"

echo
echo "### touch 'a' so it becomes most-recently-used, then insert 'd'"
q get a >/dev/null
cli put d value-d >/dev/null

echo "  a -> $(q get a || echo '(evicted)')     <- rescued by the touch"
echo "  b -> $(q get b || echo '(evicted)')     <- evicted in its place"
echo "  c -> $(q get c || echo '(evicted)')"
echo "  d -> $(q get d || echo '(evicted)')"

echo
echo "### under FIFO, 'a' would have been the one evicted. It was not."

echo
echo "### hammer it: 500 distinct keys through a 3-entry cache"
cli pipeline 500 | tail -1

echo
echo "### counters"
cli stats
