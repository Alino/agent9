#!/bin/bash
# build-libpng.sh — libpng 1.6.50 + APNG patch (vcpkg pin, apng feature)
# -> ladybird9 sysroot (tier-2).
# URL:    https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.50.tar.gz
# SHA256: 71158e53cfdf2877bc99bcab33641d78df3f48e6e0daad030afe9cb8c031aa46
# APNG:   https://downloads.sourceforge.net/project/libpng-apng/libpng16/1.6.50/libpng-1.6.50-apng.patch.gz
# SHA256: 687ddc0c7cb128a3ea58e159b5129252537c27ede0c32a93f11f03127f0c0165
#         (same patch vcpkg's ports/libpng "apng" feature applies; it defines
#          PNG_APNG_SUPPORTED directly in png.h, no dfa rebuild needed)
#
# Ladybird finds it via CMake's stock FindPNG (libpng.a + png.h + ZLIB).
# zlib comes from the sysroot (tier-1). Intel SSE paths are pure intrinsics
# with runtime dispatch — allowed under the no-assembly rule.
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
LB9="$A9/ladybird9"; SYS="$LB9/_out/deps"; SRC="$LB9/vendor/libpng"
TB="$LB9/vendor/_tarballs/libpng-1.6.50.tar.gz"
PGZ="$LB9/vendor/_tarballs/libpng-1.6.50-apng.patch.gz"
if [ ! -f "$SRC/png.h" ]; then
  mkdir -p "${TB%/*}" "$SRC"
  [ -f "$TB" ]  || curl -fsSL https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.50.tar.gz -o "$TB"
  [ -f "$PGZ" ] || curl -fsSL "https://downloads.sourceforge.net/project/libpng-apng/libpng16/1.6.50/libpng-1.6.50-apng.patch.gz" -o "$PGZ"
  echo "71158e53cfdf2877bc99bcab33641d78df3f48e6e0daad030afe9cb8c031aa46  $TB" | shasum -a 256 -c -
  echo "687ddc0c7cb128a3ea58e159b5129252537c27ede0c32a93f11f03127f0c0165  $PGZ" | shasum -a 256 -c -
  tar -xf "$TB" -C "$SRC" --strip-components=1
  gunzip -kc "$PGZ" > "$SRC/apng.patch"
fi
grep -q PNG_APNG_SUPPORTED "$SRC/png.h" || (cd "$SRC" && patch -p1 < apng.patch)
B="$LB9/_out/build-deps/libpng"
cmake -G Ninja -S "$SRC" -B "$B" \
  -DCMAKE_TOOLCHAIN_FILE="$LB9/host/toolchain.cmake" \
  -DCMAKE_PROJECT_INCLUDE= \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX="$SYS" \
  -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_FRAMEWORK=OFF \
  -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
ninja -C "$B" install
