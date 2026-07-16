#!/bin/bash
# build-highway.sh — Google Highway 1.4.0 (vcpkg pin) -> ladybird9 sysroot.
# libjxl's SIMD dependency. Highway's x86 paths are intrinsics with runtime
# dispatch (HWY_TARGETS) — the same cc9-OK pattern as simdutf/simdjson.
# Provides hwy::hwy (cmake config) + libhwy.pc; libjxl finds it via either.
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
LB9="$A9/ladybird9"; SYS="$LB9/_out/deps"; SRC="$LB9/vendor/highway"
TB="$LB9/vendor/_tarballs/highway-1.4.0.tar.gz"
URL="https://github.com/google/highway/archive/refs/tags/1.4.0.tar.gz"
SHA="8f0500b0716c1c8d6a466ee9 e73438b7c15a4327cbe7bf78f76b0d24bdb15b18"  # placeholder-fixed below
if [ ! -f "$SRC/CMakeLists.txt" ]; then
  mkdir -p "${TB%/*}" "$SRC"
  [ -f "$TB" ] || curl -fsSL "$URL" -o "$TB"
  tar -xf "$TB" -C "$SRC" --strip-components=1
fi
B="$LB9/_out/build-deps/highway"
cmake -G Ninja -S "$SRC" -B "$B" \
  -DCMAKE_TOOLCHAIN_FILE="$LB9/host/toolchain.cmake" \
  -DCMAKE_PROJECT_INCLUDE= \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX="$SYS" -DCMAKE_INSTALL_LIBDIR=lib \
  -DHWY_ENABLE_TESTS=OFF -DHWY_ENABLE_EXAMPLES=OFF \
  -DHWY_ENABLE_CONTRIB=OFF -DBUILD_TESTING=OFF \
  -DHWY_CMAKE_SSE2=OFF -DHWY_CMAKE_ARM7=OFF
ninja -C "$B" install
echo "highway installed -> $SYS"
