#!/bin/bash
# build-woff2.sh — woff2 1.0.2 (vcpkg pin) -> ladybird9 sysroot (tier-2).
# URL:    https://github.com/google/woff2/archive/refs/tags/v1.0.2.tar.gz
# SHA256: add272bb09e6384a4833ffca4896350fdb16e0ca22df68c0384773c67a175594
#
# Needs brotli in the sysroot first (build-brotli.sh). Ladybird consumes it
# via pkg_check_modules(WOFF2 ... libwoff2dec); upstream generates the .pc
# files, and this recipe patches Libs so a static link pulls woff2common +
# brotlidec transitively.
# BROTLI*_LIBRARIES are pre-seeded with libbrotlicommon.a as well — woff2's
# FindBrotli{Dec,Enc} only find the enc/dec archive, and the woff2_* tool
# links fail against static brotli (common tables live in brotlicommon).
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
LB9="$A9/ladybird9"; SYS="$LB9/_out/deps"; SRC="$LB9/vendor/woff2"
TB="$LB9/vendor/_tarballs/woff2-1.0.2.tar.gz"
if [ ! -f "$SRC/CMakeLists.txt" ]; then
  mkdir -p "${TB%/*}" "$SRC"
  [ -f "$TB" ] || curl -fsSL https://github.com/google/woff2/archive/refs/tags/v1.0.2.tar.gz -o "$TB"
  echo "add272bb09e6384a4833ffca4896350fdb16e0ca22df68c0384773c67a175594  $TB" | shasum -a 256 -c -
  tar -xf "$TB" -C "$SRC" --strip-components=1
fi
B="$LB9/_out/build-deps/woff2"
cmake -G Ninja -S "$SRC" -B "$B" \
  -DCMAKE_TOOLCHAIN_FILE="$LB9/host/toolchain.cmake" \
  -DCMAKE_PROJECT_INCLUDE= \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX="$SYS" \
  -DCANONICAL_PREFIXES=ON -DNOISY_LOGGING=OFF \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_SKIP_INSTALL_RPATH=ON \
  -DBROTLIDEC_INCLUDE_DIRS="$SYS/include" -DBROTLIENC_INCLUDE_DIRS="$SYS/include" \
  -DBROTLIDEC_LIBRARIES="$SYS/lib/libbrotlidec.a;$SYS/lib/libbrotlicommon.a" \
  -DBROTLIENC_LIBRARIES="$SYS/lib/libbrotlienc.a;$SYS/lib/libbrotlicommon.a"
ninja -C "$B" install
# Static-link correctness: libwoff2dec must drag in woff2common + brotlidec.
python3 - "$SYS/lib/pkgconfig/libwoff2dec.pc" <<'EOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
if '-lwoff2common' not in s:
    s = re.sub(r'^Libs:.*$',
               'Libs: -L${libdir} -lwoff2dec -lwoff2common',
               s, flags=re.M)
    if 'Requires' not in s:
        s += 'Requires.private: libbrotlidec\n'
    open(p, 'w').write(s)
    print('patched', p)
EOF
