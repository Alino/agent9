#!/bin/bash
# build-harfbuzz.sh — harfbuzz 10.2.0 (vcpkg pin) -> ladybird9 sysroot (tier-2).
# URL:    https://github.com/harfbuzz/harfbuzz/releases/download/10.2.0/harfbuzz-10.2.0.tar.xz
# SHA256: 620e3468faec2ea8685d32c46a58469b850ef63040b3565cde05959825b48227
#
# freetype ON (from sysroot; build-freetype.sh first). ICU OFF for the first
# pass — Ladybird's vcpkg wants harfbuzz[icu], but hb-icu is only the
# hb_icu_* unicode-funcs bridge; LibGfx links plain `harfbuzz`. Rebuild with
# HB_HAVE_ICU=ON once tier-1 ICU is proven if anything needs hb-icu.h.
# Installs lib/cmake/harfbuzz/harfbuzzConfig.cmake (harfbuzz::harfbuzz), which
# is what find_package(harfbuzz REQUIRED) resolves in config mode.
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
LB9="$A9/ladybird9"; SYS="$LB9/_out/deps"; SRC="$LB9/vendor/harfbuzz"
TB="$LB9/vendor/_tarballs/harfbuzz-10.2.0.tar.xz"
if [ ! -f "$SRC/CMakeLists.txt" ]; then
  mkdir -p "${TB%/*}" "$SRC"
  [ -f "$TB" ] || curl -fsSL https://github.com/harfbuzz/harfbuzz/releases/download/10.2.0/harfbuzz-10.2.0.tar.xz -o "$TB"
  echo "620e3468faec2ea8685d32c46a58469b850ef63040b3565cde05959825b48227  $TB" | shasum -a 256 -c -
  tar -xf "$TB" -C "$SRC" --strip-components=1
fi
B="$LB9/_out/build-deps/harfbuzz"
cmake -G Ninja -S "$SRC" -B "$B" \
  -DCMAKE_TOOLCHAIN_FILE="$LB9/host/toolchain.cmake" \
  -DCMAKE_PROJECT_INCLUDE= \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX="$SYS" \
  -DHB_HAVE_FREETYPE=ON -DHB_HAVE_ICU=OFF -DHB_BUILD_SUBSET=OFF
ninja -C "$B" install
