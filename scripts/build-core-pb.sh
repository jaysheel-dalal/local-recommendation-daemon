#!/usr/bin/env bash
# lrd_core with protobuf on, no tests or benches - the next checkpoint in the
# protocol migration.
set -u
cd "$(dirname "$0")/.."
cmake -S . -B build/corepb -DCMAKE_BUILD_TYPE=Debug -DLRD_WITH_PROTOBUF=ON \
      -DLRD_BUILD_TESTS=OFF -DLRD_BUILD_BENCH=OFF >/dev/null 2>&1 || {
  echo "configure failed"; exit 1; }
cmake --build build/corepb --target lrd_core -j"$(nproc)" 2>&1 | grep -E 'error:|warning:' | head -40
echo "CORE_EXIT=${PIPESTATUS[0]}"
