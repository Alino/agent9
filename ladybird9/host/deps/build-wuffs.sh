#!/bin/sh
# wuffs — pinned to Ladybird's vcpkg override (0.3.4). Single-file library
# compiled into the including TU; Ladybird's check is
# find_path(WUFFS_INCLUDE_DIR NAMES wuffs/wuffs-v0.3.c REQUIRED), so the only
# artifact is include/wuffs/wuffs-v0.3.c (mirrors vcpkg's overlay portfile).
. "$(dirname "$0")/common.sh"

VERSION=0.3.4
SRC_URL="https://github.com/google/wuffs-mirror-release-c/archive/refs/tags/v$VERSION.tar.gz"
SRC_SHA=d1c1e269ddbcef7c0452f8b9f645a4cb78b149f4bc679552d43b50fc41b549c3

fetch "$SRC_URL" "$SRC_SHA" "$VENDOR/wuffs"

mkdir -p "$PREFIX/include/wuffs"
cp "$VENDOR/wuffs/release/c/wuffs-v0.3.c" "$PREFIX/include/wuffs/"

echo "wuffs $VERSION installed into $PREFIX"
