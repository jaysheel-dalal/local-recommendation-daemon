#!/usr/bin/env bash
# Step 5 milestone demo: concurrent clients against the threaded daemon, and a
# graceful shutdown that actually cleans up after itself.
set -u
cd "$(dirname "$0")/.."
BIN="build/${1:-debug}/bin"
SOCK="/tmp/lrd_demo_$$.sock"
CLIENTS=8

cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

echo "### daemon: $CLIENTS worker threads, 5000-entry cache"
"$BIN/lrdd" --socket "$SOCK" --threads "$CLIENTS" --capacity 5000 &
DAEMON_PID=$!
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

echo
echo "### $CLIENTS clients, each 300 put/get pairs, all at once"
start=$(date +%s%N)
# Collect the client PIDs and wait on those specifically. A bare `wait` would
# also wait on the daemon, which is a background job of this same shell and
# never exits on its own - the script would hang forever.
pids=""
for c in $(seq 1 $CLIENTS); do
  "$BIN/lrd_cli" --socket "$SOCK" pipeline 300 > "/tmp/lrd_c$c.$$" 2>&1 &
  pids="$pids $!"
done
for pid in $pids; do wait "$pid"; done
end=$(date +%s%N)
elapsed_ms=$(( (end - start) / 1000000 ))

mismatches=$(cat /tmp/lrd_c*.$$ | grep -o '[0-9]* mismatch' | awk '{s+=$1} END {print s+0}')
echo "  wall clock: ${elapsed_ms} ms for $((CLIENTS * 600)) requests"
echo "  mismatches across all clients: $mismatches"
rm -f /tmp/lrd_c*.$$

echo
echo "### counters"
"$BIN/lrd_cli" --socket "$SOCK" stats 2>/dev/null

echo
echo "### graceful shutdown: SIGTERM (not SIGKILL)"
kill -TERM "$DAEMON_PID"
wait "$DAEMON_PID" 2>/dev/null
echo "  daemon exit status: $?"

if [ -e "$SOCK" ]; then
  echo "  FAIL: socket file $SOCK survived shutdown"
else
  echo "  socket file removed cleanly -- no stale socket for the next start"
fi
