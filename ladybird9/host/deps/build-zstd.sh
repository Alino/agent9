#!/bin/bash
# build-zstd.sh — zstd 1.5.7 (current stable; not pinned in vcpkg overrides)
# -> ladybird9 sysroot (tier-2). Consumed by curl (CURL_ZSTD=ON).
# URL:    https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
# SHA256: eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
#
# ZSTD_DISABLE_ASM: the huf x86-64 .S fast path is out per the no-assembly
# rule for the plan9 target; C fallback is used. Multithread OFF (Ladybird
# only needs streaming decompress in curl).
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
LB9="$A9/ladybird9"; SYS="$LB9/_out/deps"; SRC="$LB9/vendor/zstd"
TB="$LB9/vendor/_tarballs/zstd-1.5.7.tar.gz"
if [ ! -f "$SRC/lib/zstd.h" ]; then
  mkdir -p "${TB%/*}" "$SRC"
  [ -f "$TB" ] || curl -fsSL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz -o "$TB"
  echo "eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3  $TB" | shasum -a 256 -c -
  tar -xf "$TB" -C "$SRC" --strip-components=1
fi
B="$LB9/_out/build-deps/zstd"
cmake -G Ninja -S "$SRC/build/cmake" -B "$B" \
  -DCMAKE_TOOLCHAIN_FILE="$LB9/host/toolchain.cmake" \
  -DCMAKE_PROJECT_INCLUDE= \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX="$SYS" \
  -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON \
  -DZSTD_BUILD_TESTS=OFF -DZSTD_MULTITHREAD_SUPPORT=OFF \
  -DCMAKE_C_FLAGS="-DZSTD_DISABLE_ASM"
ninja -C "$B" install
