#!/usr/bin/env bash
# Daemon + CLI + libs, no tests or benches: the checkpoint before the test
# files have caught up with a protocol change.
set -u
cd "$(dirname "$0")/.."
cmake -S . -B build/corepb -DCMAKE_BUILD_TYPE=Debug -DLRD_WITH_PROTOBUF=ON \
      -DLRD_BUILD_TESTS=OFF -DLRD_BUILD_BENCH=OFF >/dev/null 2>&1 || {
  echo "configure failed"; exit 1; }
cmake --build build/corepb -j"$(nproc)" 2>&1 | grep -E 'error:|warning:' | head -40
echo "EXIT=${PIPESTATUS[0]}"
