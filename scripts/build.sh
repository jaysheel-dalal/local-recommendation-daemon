#!/usr/bin/env bash
# Build one preset (default: debug) and surface only diagnostics.
set -u
cd "$(dirname "$0")/.."
preset="${1:-debug}"
cmake --preset "$preset" >/dev/null || exit 1
# Sanitizer builds link with much larger object files and can exhaust memory on
# a small machine when every core links at once - which showed up here as a bare
# "ld returned 1 exit status" that vanished on a serial retry. Halve the job
# count for those presets rather than chase a phantom link error.
jobs=$(nproc)
case "$preset" in
  tsan|asan) jobs=$(( jobs > 2 ? jobs / 2 : 1 ));;
esac
cmake --build --preset "$preset" -j"$jobs" 2>&1 | grep -E "error:|warning:|Error [0-9]" | head -50
echo "BUILD_EXIT=${PIPESTATUS[0]}"
