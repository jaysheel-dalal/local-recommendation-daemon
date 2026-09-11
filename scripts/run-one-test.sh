#!/usr/bin/env bash
# Run a single test binary and show only case results.
set -u
cd "$(dirname "$0")/.."
preset="${2:-debug}"
timeout 180 "./build/$preset/bin/$1" 2>&1 | grep -E '(^\[|case)'
echo "exit=${PIPESTATUS[0]}"
