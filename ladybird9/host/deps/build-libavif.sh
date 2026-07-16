#!/bin/bash
# build-libavif.sh — libavif 1.3.0 (vcpkg pin) -> ladybird9 sysroot.
# Decoder only, via SYSTEM dav1d (build-dav1d.sh first). No apps/tests/examples,
# no libyuv (built-in color path — slower, correct). Installs libavifConfig.cmake
# which exports the `avif` target LibImageDecoders links.
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
LB9="$A9/ladybird9"; SYS="$LB9/_out/deps"; SRC="$LB9/vendor/libavif"
TB="$LB9/vendor/_tarballs/libavif-1.3.0.tar.gz"
URL="https://github.com/AOMediaCodec/libavif/archive/refs/tags/v1.3.0.tar.gz"
if [ ! -f "$SRC/CMakeLists.txt" ]; then
  mkdir -p "${TB%/*}" "$SRC"
  [ -f "$TB" ] || curl -fsSL "$URL" -o "$TB"
  tar -xf "$TB" -C "$SRC" --strip-components=1
fi
B="$LB9/_out/build-deps/libavif"
cmake -G Ninja -S "$SRC" -B "$B" \
  -DCMAKE_TOOLCHAIN_FILE="$LB9/host/toolchain.cmake" \
  -DCMAKE_PROJECT_INCLUDE= \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX="$SYS" -DCMAKE_INSTALL_LIBDIR=lib \
  -DAVIF_CODEC_DAV1D=SYSTEM \
  -DAVIF_LIBYUV=OFF -DAVIF_LIBSHARPYUV=OFF \
  -DAVIF_BUILD_APPS=OFF -DAVIF_BUILD_TESTS=OFF \
  -DAVIF_BUILD_EXAMPLES=OFF -DAVIF_ENABLE_WERROR=OFF
ninja -C "$B" install
echo "libavif installed -> $SYS"
