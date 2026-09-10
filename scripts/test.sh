#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
preset="${1:-debug}"
ctest --preset "$preset" 2>&1 | tail -25
