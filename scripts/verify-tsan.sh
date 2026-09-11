#!/usr/bin/env bash
# Negative control for the "TSan clean" claim.
#
# A clean ThreadSanitizer run only means something if TSan would have caught a
# race in this code had one existed. This deliberately races an unsynchronised
# int across the pool's workers and asserts that TSan reports it.
set -u
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)

cat > "$tmp/race.cpp" <<'EOF'
#include "lrd/concurrency/thread_pool.hpp"
#include <cstdio>
int main() {
    lrd::concurrency::ThreadPool pool(8, 4096);
    int unsynchronised = 0;          // deliberately not atomic
    for (int i = 0; i < 2000; ++i) {
        (void)pool.post([&unsynchronised] { ++unsynchronised; });
    }
    pool.shutdown();
    std::printf("counter=%d\n", unsynchronised);
}
EOF

g++ -std=c++20 -g -fsanitize=thread -fno-omit-frame-pointer -pthread \
    -I include "$tmp/race.cpp" build/tsan/lib/liblrd_core.a -o "$tmp/race" 2>&1 | head -5

echo "--- running the deliberately racy program under TSan ---"
"$tmp/race" > "$tmp/out.txt" 2>&1
echo "exit=$?"

if grep -q "WARNING: ThreadSanitizer: data race" "$tmp/out.txt"; then
  echo "RESULT: TSan detected the injected race -- the clean runs are meaningful."
  grep -m1 -A3 "WARNING: ThreadSanitizer" "$tmp/out.txt"
else
  echo "RESULT: TSan did NOT detect the injected race -- clean runs prove nothing!"
  head -20 "$tmp/out.txt"
fi
rm -rf "$tmp"
