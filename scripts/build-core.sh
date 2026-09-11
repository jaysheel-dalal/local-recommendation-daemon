#!/usr/bin/env bash
# Builds just lrd_core, protobuf off. Used during a protocol migration to
# validate the core before the dependent targets have caught up.
set -u
cd "$(dirname "$0")/.."
cmake -S . -B build/core -DCMAKE_BUILD_TYPE=Debug -DLRD_WITH_PROTOBUF=OFF \
      -DLRD_BUILD_TESTS=OFF -DLRD_BUILD_BENCH=OFF >/dev/null 2>&1 || {
  echo "configure failed"; exit 1; }
cmake --build build/core --target lrd_core -j"$(nproc)" 2>&1 | grep -E 'error:|warning:' | head -40
echo "CORE_EXIT=${PIPESTATUS[0]}"
