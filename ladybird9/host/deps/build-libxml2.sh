#!/bin/bash
# build-libxml2.sh — libxml2 2.13.8 (vcpkg pin) -> ladybird9 sysroot (tier-2).
# URL:    https://download.gnome.org/sources/libxml2/2.13/libxml2-2.13.8.tar.xz
# SHA256: 277294cb33119ab71b2bc81f2f445e9bc9435b893ad15bb2cd2b0e859a0ee84a
#
# First pass: PYTHON/LZMA/HTTP/ICU off (per port plan), zlib from sysroot.
# MODULES off — no dlopen on the target. Installs libxml2-config cmake
# package + libxml-2.0.pc; Ladybird uses find_package(LibXml2) ->
# LibXml2::LibXml2 which the config file provides.
#
# HAVE_GETENTROPY=0: check_function_exists() is compile-only under this
# toolchain (CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY), so it
# false-positives on functions cc9's libc lacks; dict.c then calls a
# nonexistent getentropy(). Pre-seed the cache to force the /dev/urandom
# (fd) fallback path.
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
LB9="$A9/ladybird9"; SYS="$LB9/_out/deps"; SRC="$LB9/vendor/libxml2"
TB="$LB9/vendor/_tarballs/libxml2-2.13.8.tar.xz"
if [ ! -f "$SRC/CMakeLists.txt" ]; then
  mkdir -p "${TB%/*}" "$SRC"
  [ -f "$TB" ] || curl -fsSL https://download.gnome.org/sources/libxml2/2.13/libxml2-2.13.8.tar.xz -o "$TB"
  echo "277294cb33119ab71b2bc81f2f445e9bc9435b893ad15bb2cd2b0e859a0ee84a  $TB" | shasum -a 256 -c -
  tar -xf "$TB" -C "$SRC" --strip-components=1
fi
B="$LB9/_out/build-deps/libxml2"
cmake -G Ninja -S "$SRC" -B "$B" \
  -DCMAKE_TOOLCHAIN_FILE="$LB9/host/toolchain.cmake" \
  -DCMAKE_PROJECT_INCLUDE= \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX="$SYS" \
  -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_HTTP=OFF \
  -DLIBXML2_WITH_ICU=OFF -DLIBXML2_WITH_ZLIB=ON -DLIBXML2_WITH_MODULES=OFF \
  -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_TESTS=OFF \
  -DHAVE_GETENTROPY=0
ninja -C "$B" install
