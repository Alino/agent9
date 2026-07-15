#!/bin/bash
# build-nghttp2.sh — nghttp2 1.68.0 (vcpkg pin) -> ladybird9 sysroot (tier-2).
# URL:    https://github.com/nghttp2/nghttp2/releases/download/v1.68.0/nghttp2-1.68.0.tar.gz
# SHA256: 2c16ffc588ad3f9e2613c3fad72db48ecb5ce15bc362fcc85b342e48daf51013
#
# Lib-only (ENABLE_LIB_ONLY): just libnghttp2.a for curl's -DUSE_NGHTTP2=ON.
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
LB9="$A9/ladybird9"; SYS="$LB9/_out/deps"; SRC="$LB9/vendor/nghttp2"
TB="$LB9/vendor/_tarballs/nghttp2-1.68.0.tar.gz"
if [ ! -f "$SRC/CMakeLists.txt" ]; then
  mkdir -p "${TB%/*}" "$SRC"
  [ -f "$TB" ] || curl -fsSL https://github.com/nghttp2/nghttp2/releases/download/v1.68.0/nghttp2-1.68.0.tar.gz -o "$TB"
  echo "2c16ffc588ad3f9e2613c3fad72db48ecb5ce15bc362fcc85b342e48daf51013  $TB" | shasum -a 256 -c -
  tar -xf "$TB" -C "$SRC" --strip-components=1
fi
B="$LB9/_out/build-deps/nghttp2"
cmake -G Ninja -S "$SRC" -B "$B" \
  -DCMAKE_TOOLCHAIN_FILE="$LB9/host/toolchain.cmake" \
  -DCMAKE_PROJECT_INCLUDE= \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX="$SYS" \
  -DENABLE_LIB_ONLY=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
  -DENABLE_DOC=OFF
ninja -C "$B" install
