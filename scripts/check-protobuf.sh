#!/usr/bin/env bash
# Environment check for Phase 2 step 8 (ProtobufCodec).
echo "--- packages available ---"
apt-cache policy protobuf-compiler libprotobuf-dev pkg-config 2>/dev/null \
  | grep -E '^[a-z].*:$|Installed:|Candidate:'
echo
echo "--- protoc present? ---"
(protoc --version) 2>&1
echo
echo "--- CMake's own FindProtobuf module (the fallback if protobuf's config breaks) ---"
find /usr/share/cmake* -name 'FindProtobuf.cmake' 2>/dev/null | head -3
echo
echo "--- cmake ---"
cmake --version | head -1
echo
echo "--- disk headroom ---"
df -h /usr | tail -1
