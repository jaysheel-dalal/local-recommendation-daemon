#!/usr/bin/env bash
# The step 11 milestone, made checkable.
#
# Installs the SDK to a throwaway prefix, then builds the example application
# from a *separate* directory using find_package(lrd) - with no access to this
# source tree at all. If the SDK leaks an internal header, or forgets to install
# one it needs, or exports a target with a source-tree path in it, this fails.
#
# "The example links only the SDK" is easy to claim from inside the build. This
# is the version that cannot be fooled.
set -u
cd "$(dirname "$0")/.."
SRC=$(pwd)
tmp=$(mktemp -d)
prefix="$tmp/prefix"

echo "### configuring and installing to $prefix"
cmake -S "$SRC" -B "$tmp/build" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$prefix" -DLRD_BUILD_TESTS=OFF \
      -DLRD_BUILD_BENCH=OFF -DLRD_BUILD_EXAMPLES=OFF >/dev/null 2>&1 || {
  echo "FAIL: configure"; rm -rf "$tmp"; exit 1; }
cmake --build "$tmp/build" --target install -j"$(nproc)" >"$tmp/install.log" 2>&1 || {
  echo "FAIL: install"; tail -20 "$tmp/install.log"; rm -rf "$tmp"; exit 1; }

echo
echo "### what got installed"
find "$prefix" -type f | sed "s|$prefix/||" | sort

echo
echo "### headers a consumer can see"
find "$prefix/include" -name '*.hpp' | sed "s|$prefix/include/||" | sort

echo
echo "### building the example out-of-tree against find_package(lrd)"
mkdir -p "$tmp/consumer"
cp "$SRC/examples/recommender_app.cpp" "$tmp/consumer/"

cat > "$tmp/consumer/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(lrd_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(lrd REQUIRED)

add_executable(consumer recommender_app.cpp)
target_link_libraries(consumer PRIVATE lrd::sdk)
EOF

if cmake -S "$tmp/consumer" -B "$tmp/consumer/build" \
        -DCMAKE_PREFIX_PATH="$prefix" >"$tmp/consumer.log" 2>&1 &&
   cmake --build "$tmp/consumer/build" -j"$(nproc)" >>"$tmp/consumer.log" 2>&1; then
  echo "RESULT: the example builds against the installed SDK alone."
  ls -l "$tmp/consumer/build/consumer" | awk '{print "  binary:", $NF, $5, "bytes"}'
else
  echo "RESULT: FAILED"
  tail -30 "$tmp/consumer.log"
  rm -rf "$tmp"; exit 1
fi

rm -rf "$tmp"
