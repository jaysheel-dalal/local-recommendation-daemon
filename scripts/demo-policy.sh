#!/usr/bin/env bash
# Step 10 demo: exposure caps and frequency limits, visible from a client.
set -u
cd "$(dirname "$0")/.."
BIN="build/${1:-release}/bin"
SOCK="/tmp/lrd_pol_$$.sock"
cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

cli() { "$BIN/lrd_cli" --socket "$SOCK" "$@" 2>&1; }
q()   { "$BIN/lrd_cli" --socket "$SOCK" "$@" 2>/dev/null; }

start() {
  "$BIN/lrdd" --socket "$SOCK" --capacity 500 --shards 8 "$@" >/dev/null 2>&1 &
  DAEMON_PID=$!
  for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done
}
stop() { kill "$DAEMON_PID" 2>/dev/null; wait "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }

echo "=============================================================="
echo " exposure cap: 3 lifetime shows per item, 4 items published"
echo "=============================================================="
start --exposure-cap 3
for i in 1 2 3 4; do
  cli put-item "$i" tech "0.$((10 - i))" --advertiser "adv-$i" >/dev/null
done

echo "asking for one item, six times in a row:"
for n in 1 2 3 4 5 6; do
  printf "  request %s -> " "$n"
  result=$(q recommend --signal 'tech=1.0' --count 1 | tail -1)
  if [ -z "$result" ]; then echo "(no eligible candidates)"; else echo "$result"; fi
done
echo
echo "counters:"
q stats | tail -6
stop

echo
echo "=============================================================="
echo " dry run: reports the slate without spending it"
echo "=============================================================="
start --exposure-cap 1
cli put-item 1 tech 0.9 >/dev/null

echo "three dry runs:"
for n in 1 2 3; do
  printf "  dry %s -> " "$n"
  q recommend --signal 'tech=1.0' --count 1 --dry-run | tail -1
done
echo "then one live run, then a fourth dry run:"
printf "  live  -> "; q recommend --signal 'tech=1.0' --count 1 | tail -1
printf "  dry 4 -> "
result=$(q recommend --signal 'tech=1.0' --count 1 --dry-run | tail -1)
if [ -z "$result" ]; then echo "(no eligible candidates -- cap now spent)"; else echo "$result"; fi
stop

echo
echo "=============================================================="
echo " frequency limit: 2 shows per item per 60s window"
echo "=============================================================="
start --frequency-limit 2 --frequency-window 60
cli put-item 1 tech 0.9 --advertiser solo >/dev/null

echo "four requests inside the same window:"
for n in 1 2 3 4; do
  printf "  request %s -> " "$n"
  result=$(q recommend --signal 'tech=1.0' --count 1 | tail -1)
  if [ -z "$result" ]; then echo "(frequency limited)"; else echo "$result"; fi
done
echo
echo "counters:"
q stats | tail -6
stop

echo
echo "=============================================================="
echo " delete and republish does NOT reset the cap"
echo "=============================================================="
start --exposure-cap 1
cli put-item 1 tech 0.9 >/dev/null
printf "  first show      -> "; q recommend --signal 'tech=1.0' --count 1 | tail -1
cli del-item 1 >/dev/null
cli put-item 1 tech 0.9 >/dev/null
printf "  after republish -> "
result=$(q recommend --signal 'tech=1.0' --count 1 | tail -1)
if [ -z "$result" ]; then echo "(still capped -- counters outlive the item)"; else echo "$result"; fi
