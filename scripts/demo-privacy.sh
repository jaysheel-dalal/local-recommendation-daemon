#!/usr/bin/env bash
# Step 12 demo: exported metrics with and without the noise mechanism.
set -u
cd "$(dirname "$0")/.."
BIN="build/${1:-release}/bin"
SOCK="/tmp/lrd_priv_$$.sock"
cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

cli() { "$BIN/lrd_cli" --socket "$SOCK" "$@" 2>/dev/null; }

start() {
  "$BIN/lrdd" --socket "$SOCK" --capacity 500 --shards 8 "$@" >/dev/null 2>&1 &
  DAEMON_PID=$!
  for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done
}
stop() { kill "$DAEMON_PID" 2>/dev/null; wait "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }

# Generates identical activity in both arms so the numbers are comparable.
generate_load() {
  cli seed 60 >/dev/null
  for _ in $(seq 1 40); do
    cli recommend --signal 'tech=1.0,sport=0.4' --count 3 >/dev/null
  done
  for i in $(seq 1 25); do cli get-item "$i" >/dev/null; done
  for i in $(seq 900 910); do cli get-item "$i" >/dev/null; done   # misses
}

echo "=============================================================="
echo " privacy OFF -- exact counters"
echo "=============================================================="
start
generate_load
cli stats | head -9
stop

echo
echo "=============================================================="
echo " privacy ON -- epsilon 1.0, suppress <= 5, round to 10"
echo "=============================================================="
start --privacy --privacy-seed 424242
generate_load
cli stats | head -9

echo
echo "### the averaging attack: 12 reads of the same counter, same epoch"
for _ in $(seq 1 12); do
  cli stats | grep '^recommends' | awk '{printf "%s ", $2}'
done
echo
echo "### identical every time -- there is nothing to average away"
stop

echo
echo "=============================================================="
echo " the same load, ten different seeds, rounding off"
echo "=============================================================="
echo " (rounding to 10 hides noise of +/-1-3 at these counts, so it is"
echo "  switched off here to show the raw draw. epsilon 0.5 -> scale 2.)"
echo
printf "  exact recommends = 40; published: "
for seed in 11 22 33 44 55 66 77 88 99 110; do
  start --privacy --privacy-seed "$seed" --privacy-round 1 --privacy-epsilon 0.5
  generate_load
  printf "%s " "$(cli stats | grep '^recommends' | awk '{print $2}')"
  stop
done
echo
echo
echo " Each is one independent draw around the true value of 40. An observer"
echo " seeing any single number cannot tell 40 from 38 or 43 - which is the"
echo " point. What they could do is watch across epochs and average, and that"
echo " is the budget cost documented in docs/privacy.md."
