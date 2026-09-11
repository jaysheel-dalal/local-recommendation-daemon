#!/usr/bin/env bash
# Reports test results for each preset that has been built.
set -u
cd "$(dirname "$0")/.."
for p in debug release tsan asan; do
  printf "%-8s " "$p"
  if [ ! -x "build/$p/bin/test_sdk" ]; then
    echo "not built"
    continue
  fi
  if [ "$p" = "release" ]; then
    echo "built (tests not run for release)"
    continue
  fi
  ctest --preset "$p" 2>/dev/null | grep -E "tests passed|tests failed" | head -1
done
