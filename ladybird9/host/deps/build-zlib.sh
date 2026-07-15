#!/bin/sh
# zlib — pinned to Ladybird's vcpkg override (1.3.1). Hand-compiled with
# cc9-cc (same sources python9 built; here including the gz* file-I/O TUs so
# libz.a is complete). No CMake package needed: find_package(ZLIB) uses the
# builtin FindZLIB module (zlib.h/zconf.h + libz.a in the sysroot).
. "$(dirname "$0")/common.sh"

VERSION=1.3.1
SRC_URL="https://github.com/madler/zlib/releases/download/v$VERSION/zlib-$VERSION.tar.gz"
SRC_SHA=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23

fetch "$SRC_URL" "$SRC_SHA" "$VENDOR/zlib"

cd "$VENDOR/zlib"
rm -f *.o
for f in adler32 compress crc32 deflate gzclose gzlib gzread gzwrite \
         infback inffast inflate inftrees trees uncompr zutil; do
	"$CC9CC" -O2 -DZ_HAVE_UNISTD_H -c "$f.c" -o "$f.o"   # unistd: gz* need lseek
done
"$AR" rcs "$PREFIX/lib/libz.a" *.o
cp zlib.h zconf.h "$PREFIX/include/"

echo "zlib $VERSION installed into $PREFIX"
