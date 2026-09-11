#!/usr/bin/env bash
# Step 0 smoke check: proves the build system, the binaries and CTest all work.
set -u
cd "$(dirname "$0")/.."
BIN=build/debug/bin

run() {
  echo "--- $* ---"
  "$@"
  echo "exit=$?"
}

run "$BIN/lrdd" --version
run "$BIN/lrdd" --bogus
run "$BIN/lrd_bench"
echo "--- ctest ---"
ctest --preset debug 2>&1 | tail -5
