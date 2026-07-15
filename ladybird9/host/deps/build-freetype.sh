#!/bin/bash
# build-freetype.sh — freetype 2.13.3 (vcpkg pin) -> ladybird9 sysroot (tier-2).
# URL:    https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz
# SHA256: 0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
#
# zlib + png from the sysroot (build libpng first). Brotli (WOFF via
# freetype — Ladybird decodes WOFF2 through woff2/brotli itself) and bzip2
# deferred; harfbuzz OFF to break the ft<->hb cycle (vcpkg does the same).
# Consumers: harfbuzz, Skia. Installs freetype-config cmake package + .pc.
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
LB9="$A9/ladybird9"; SYS="$LB9/_out/deps"; SRC="$LB9/vendor/freetype"
TB="$LB9/vendor/_tarballs/freetype-2.13.3.tar.xz"
if [ ! -f "$SRC/CMakeLists.txt" ]; then
  mkdir -p "${TB%/*}" "$SRC"
  [ -f "$TB" ] || curl -fsSL https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz -o "$TB"
  echo "0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289  $TB" | shasum -a 256 -c -
  tar -xf "$TB" -C "$SRC" --strip-components=1
fi
B="$LB9/_out/build-deps/freetype"
cmake -G Ninja -S "$SRC" -B "$B" \
  -DCMAKE_TOOLCHAIN_FILE="$LB9/host/toolchain.cmake" \
  -DCMAKE_PROJECT_INCLUDE= \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX="$SYS" \
  -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON \
  -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_HARFBUZZ=ON
ninja -C "$B" install
