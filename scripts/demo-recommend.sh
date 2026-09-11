#!/usr/bin/env bash
# Step 9 milestone: a ranked, filtered slate from a local user signal.
set -u
cd "$(dirname "$0")/.."
BIN="build/${1:-corepb}/bin"
SOCK="/tmp/lrd_rec_$$.sock"
cleanup() { kill "$DAEMON_PID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

cli() { "$BIN/lrd_cli" --socket "$SOCK" "$@" 2>&1; }

echo "### daemon"
"$BIN/lrdd" --socket "$SOCK" --capacity 2000 --shards 16 >/dev/null 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

echo
echo "### seed 120 items across 6 categories, 4 advertisers, ages 0-10 days"
cli seed 120 | tail -1

echo
echo "### a user who likes tech strongly and sport a little"
cli recommend --signal 'tech=1.0,sport=0.3' --count 5

echo
echo "### the same user, excluding tech entirely (hard filter)"
cli recommend --signal 'tech=1.0,sport=0.3' --exclude tech --count 5

echo
echo "### a brand-new user with no signal at all"
cli recommend --count 5

echo
echo "### note the advertiser column: the diversity penalty spreads them out"
echo "### (advertiser repeat factor 0.3 is stricter than category's 0.5)"

echo
echo "### one item fetched by id"
cli get-item 7

echo
echo "### counters"
cli stats
