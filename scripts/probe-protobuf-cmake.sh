#!/usr/bin/env bash
# Does find_package(Protobuf) work under CMake 4.2? Protobuf 3.21's bundled
# config may declare a cmake_minimum_required below CMake 4's hard floor, which
# would make CONFIG mode fail. Test both modes before designing around either.
set -u
tmp=$(mktemp -d)
mkdir -p "$tmp/build"

cat > "$tmp/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Protobuf REQUIRED ${PROBE_MODE})
message(STATUS "protobuf version: ${Protobuf_VERSION}")
message(STATUS "protoc: ${Protobuf_PROTOC_EXECUTABLE}")
message(STATUS "libraries: ${Protobuf_LIBRARIES}")
if(TARGET protobuf::libprotobuf)
  message(STATUS "imported target protobuf::libprotobuf: yes")
endif()
if(COMMAND protobuf_generate_cpp)
  message(STATUS "protobuf_generate_cpp available: yes")
endif()
EOF

for mode in MODULE CONFIG; do
  echo "=================== find_package(... $mode) ==================="
  rm -rf "$tmp/build"; mkdir -p "$tmp/build"
  if cmake -S "$tmp" -B "$tmp/build" -DPROBE_MODE=$mode >"$tmp/out.txt" 2>&1; then
    grep -E 'protobuf version|protoc:|libraries|imported target|available' "$tmp/out.txt"
    echo "RESULT: $mode works"
  else
    echo "RESULT: $mode FAILED"
    grep -iE 'error|minimum required|deprecat' "$tmp/out.txt" | head -6
  fi
done
rm -rf "$tmp"
