#!/bin/bash
# build-brotli.sh — brotli 1.2.0 (vcpkg pin) -> ladybird9 sysroot (tier-2).
# URL:    https://github.com/google/brotli/archive/refs/tags/v1.2.0.tar.gz
# SHA256: 816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec
#
# Ladybird finds brotli via pkg_check_modules(libbrotlienc libbrotlidec
# libbrotlicommon) — brotli's own cmake install writes those .pc files.
# woff2 and curl (CURL_BROTLI=ON) consume it from the sysroot too.
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
LB9="$A9/ladybird9"; SYS="$LB9/_out/deps"; SRC="$LB9/vendor/brotli"
TB="$LB9/vendor/_tarballs/brotli-1.2.0.tar.gz"
if [ ! -f "$SRC/CMakeLists.txt" ]; then
  mkdir -p "${TB%/*}" "$SRC"
  [ -f "$TB" ] || curl -fsSL https://github.com/google/brotli/archive/refs/tags/v1.2.0.tar.gz -o "$TB"
  echo "816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec  $TB" | shasum -a 256 -c -
  tar -xf "$TB" -C "$SRC" --strip-components=1
fi
B="$LB9/_out/build-deps/brotli"
cmake -G Ninja -S "$SRC" -B "$B" \
  -DCMAKE_TOOLCHAIN_FILE="$LB9/host/toolchain.cmake" \
  -DCMAKE_PROJECT_INCLUDE= \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX="$SYS" \
  -DBROTLI_BUILD_TOOLS=OFF -DBROTLI_DISABLE_TESTS=ON
ninja -C "$B" install
