#!/bin/sh
# libtommath — pinned to Ladybird's vcpkg override (1.3.0). Pure C, CMake.
# Ladybird finds it via pkg-config (pkg_check_modules(libtommath ... libtommath)),
# so the load-bearing artifact is lib/pkgconfig/libtommath.pc; we write it
# ourselves (upstream only generates it from the makefile path, not CMake).
. "$(dirname "$0")/common.sh"

VERSION=1.3.0
SRC_URL="https://github.com/libtom/libtommath/releases/download/v$VERSION/ltm-$VERSION.tar.xz"
SRC_SHA=296272d93435991308eb73607600c034b558807a07e829e751142e65ccfa9d08

fetch "$SRC_URL" "$SRC_SHA" "$VENDOR/libtommath"
cmake_dep "$VENDOR/libtommath"

cat > "$PREFIX/lib/pkgconfig/libtommath.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: LibTomMath
Description: multiple-precision integer library (ladybird9 static build)
Version: $VERSION
Libs: -L\${libdir} -ltommath
Cflags: -I\${includedir}
EOF

echo "libtommath $VERSION installed into $PREFIX"
