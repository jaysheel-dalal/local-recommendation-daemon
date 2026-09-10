#!/usr/bin/env bash
# Build one preset (default: debug) and surface only diagnostics.
set -u
cd "$(dirname "$0")/.."
preset="${1:-debug}"
cmake --preset "$preset" >/dev/null || exit 1
cmake --build --preset "$preset" -j"$(nproc)" 2>&1 | grep -E "error:|warning:|Error [0-9]" | head -50
echo "BUILD_EXIT=${PIPESTATUS[0]}"
