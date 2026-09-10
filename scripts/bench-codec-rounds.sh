#!/usr/bin/env bash
# Several interleaved rounds, because one run of the codec comparison gave a
# 36% gap and the next reversed it. On a host with this much variance, a single
# run is an anecdote.
set -u
cd "$(dirname "$0")/.."
for r in 1 2 3; do
  echo "########## round $r ##########"
  bash scripts/bench-codec-e2e.sh 2>&1 | grep -E '^===|threads=8'
done
