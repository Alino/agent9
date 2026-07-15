#!/bin/sh
# sqlite3 — amalgamation, pinned to Ladybird's vcpkg override (3.52.0).
# One TU compiled with cc9-cc -> libsqlite3.a. No CMake package needed:
# find_package(SQLite3) uses CMake's builtin FindSQLite3 module, which finds
# include/sqlite3.h + lib/libsqlite3.a via CMAKE_FIND_ROOT_PATH.
# Flags mirror python9's proven cc9 sqlite build (3.46 there).
. "$(dirname "$0")/common.sh"

VERSION=3.52.0
SRC_URL="https://sqlite.org/2026/sqlite-amalgamation-3520000.zip"
SRC_SHA=45d0fea15971dd1300e5b509f48ca134621e10d9297713b64618fbca21b325fa

fetch "$SRC_URL" "$SRC_SHA" "$VENDOR/sqlite3"

"$CC9CC" -O2 -c "$VENDOR/sqlite3/sqlite3.c" -o "$VENDOR/sqlite3/sqlite3.o" \
	-DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1
"$AR" rcs "$PREFIX/lib/libsqlite3.a" "$VENDOR/sqlite3/sqlite3.o"
cp "$VENDOR/sqlite3/sqlite3.h" "$VENDOR/sqlite3/sqlite3ext.h" "$PREFIX/include/"

echo "sqlite3 $VERSION installed into $PREFIX"
