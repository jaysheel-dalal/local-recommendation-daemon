#!/usr/bin/env bash
# Sanity check: confirm the tests actually run cases, and that the harness
# genuinely reports failures rather than passing vacuously.
set -u
cd "$(dirname "$0")/.."
echo "### test_unix_socket case list"
./build/debug/bin/test_unix_socket
echo
echo "### harness self-check: a deliberately failing case must exit non-zero"
tmp=$(mktemp -d)
cat > "$tmp/t.cpp" <<'EOF'
#include "test_harness.hpp"
LRD_TEST("this must fail") { LRD_CHECK_EQ(1, 2); }
EOF
g++ -std=c++20 -I tests -o "$tmp/t" "$tmp/t.cpp" tests/test_main.cpp 2>&1 | head -5
"$tmp/t"
echo "self_check_exit=$? (expected 1)"
rm -rf "$tmp"
