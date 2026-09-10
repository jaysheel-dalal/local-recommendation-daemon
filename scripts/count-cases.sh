#!/usr/bin/env bash
cd "$(dirname "$0")/.."
total=0
for t in test_version test_fd test_io test_unix_socket; do
  line=$("./build/debug/bin/$t" 2>/dev/null | grep 'case(s)')
  n=$(echo "$line" | awk '{print $1}')
  total=$((total + n))
  printf "%-20s %s\n" "$t" "$line"
done
echo "TOTAL CASES: $total"
