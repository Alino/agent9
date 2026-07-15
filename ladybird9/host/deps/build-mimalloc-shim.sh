#!/bin/sh
# mimalloc — SHIM, not the real allocator (see port/mimalloc-shim/mimalloc.h
# for why). Nothing to fetch: the source is committed in port/mimalloc-shim/.
# Installs libmimalloc.a + mimalloc.h + a CONFIG package so Ladybird's
# find_package(mimalloc CONFIG REQUIRED) and `target_link_libraries(AK ... mimalloc)`
# resolve unchanged.
. "$(dirname "$0")/common.sh"

SHIM="$LB9/port/mimalloc-shim"

# object goes to gitignored vendor/, keeping the committed shim dir clean
"$CC9CC" -O2 -c "$SHIM/mimalloc-shim.c" -o "$VENDOR/mimalloc-shim.o"
"$AR" rcs "$PREFIX/lib/libmimalloc.a" "$VENDOR/mimalloc-shim.o"
cp "$SHIM/mimalloc.h" "$PREFIX/include/"
mkdir -p "$PREFIX/lib/cmake/mimalloc"
cp "$SHIM/mimalloc-config.cmake" "$PREFIX/lib/cmake/mimalloc/"

echo "mimalloc shim installed into $PREFIX"
