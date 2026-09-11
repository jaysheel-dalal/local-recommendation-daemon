#!/usr/bin/env bash
cd "$(dirname "$0")/.."
total=0
for t in test_version test_fd test_io test_unix_socket test_wire test_codec test_protobuf_codec test_framing test_lru_cache test_locked_cache test_sharded_cache test_scorer test_exposure_store test_thread_pool test_handler test_server test_sdk; do
  line=$("./build/debug/bin/$t" 2>/dev/null | grep 'case(s)')
  n=$(echo "$line" | awk '{print $1}')
  total=$((total + n))
  printf "%-20s %s\n" "$t" "$line"
done
echo "TOTAL CASES: $total"
