#!/usr/bin/env bash
# Configure and build every preset, then run the test suite under each of the
# checked builds. This is the "is the tree healthy" command.
set -u
cd "$(dirname "$0")/.."
rc=0
for preset in debug release tsan asan; do
  echo "=================== $preset ==================="
  cmake --preset "$preset" >/dev/null 2>&1 || { echo "CONFIGURE FAILED"; rc=1; continue; }
  if ! cmake --build --preset "$preset" -j"$(nproc)" 2>&1 | grep -E 'error:|warning:|Error [0-9]' ; then
    echo "build: clean"
  else
    echo "build: PROBLEMS"; rc=1
  fi
  case "$preset" in
    release) ;;
    *) ctest --preset "$preset" 2>&1 | tail -3 || rc=1 ;;
  esac
done
exit $rc
