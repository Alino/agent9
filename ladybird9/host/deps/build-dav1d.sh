#!/bin/bash
# build-dav1d.sh — dav1d 1.5.1 (vcpkg pin) -> ladybird9 sysroot.
# libavif's AV1 decoder. dav1d is a Meson project (the only one in the dep pile);
# host/deps/cc9-meson-cross.ini is the cc9 cross-file. asm OFF (dav1d's NASM/GAS
# can't go through cc9/LLVM->a.out; the C fallback decodes correctly, slower).
# Installs dav1d.pc + libdav1d.a so libavif's find_package/pkg finds it.
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
LB9="$A9/ladybird9"; SYS="$LB9/_out/deps"; SRC="$LB9/vendor/dav1d"
HERE="$LB9/host/deps"
TB="$LB9/vendor/_tarballs/dav1d-1.5.1.tar.gz"
URL="https://code.videolan.org/videolan/dav1d/-/archive/1.5.1/dav1d-1.5.1.tar.gz"
MESON="${MESON:-$HOME/Library/Python/3.14/bin/meson}"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
if [ ! -f "$SRC/meson.build" ]; then
  mkdir -p "${TB%/*}" "$SRC"
  [ -f "$TB" ] || curl -fsSL "$URL" -o "$TB"
  tar -xf "$TB" -C "$SRC" --strip-components=1
fi
# Materialize the cross-file with real absolute tool paths.
CROSS="$LB9/_out/build-deps/cc9-cross.ini"
mkdir -p "${CROSS%/*}"
sed -e "s#CC9CC_PATH#$A9/servo9/host/cc9-cc#" \
    -e "s#CC9CXX_PATH#$A9/servo9/host/cc9-c++#" \
    -e "s#LLVM_AR_PATH#$LLVM/llvm-ar#" \
    -e "s#LLVM_STRIP_PATH#$LLVM/llvm-strip#" \
    "$HERE/cc9-meson-cross.ini" > "$CROSS"

B="$LB9/_out/build-deps/dav1d"
rm -rf "$B"
"$MESON" setup "$B" "$SRC" \
  --cross-file "$CROSS" \
  --prefix "$SYS" --libdir lib \
  --buildtype release --default-library static \
  -Denable_asm=false -Denable_tools=false -Denable_tests=false \
  -Denable_examples=false
"$MESON" compile -C "$B"
"$MESON" install -C "$B"
echo "dav1d installed -> $SYS"
