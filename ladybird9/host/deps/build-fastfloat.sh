#!/bin/sh
# fast-float — pinned to Ladybird's vcpkg override (8.1.0). Header-only; the
# CMake install also provides the CONFIG package Ladybird requires:
# find_package(FastFloat CONFIG REQUIRED) -> FastFloat::fast_float.
. "$(dirname "$0")/common.sh"

VERSION=8.1.0
SRC_URL="https://github.com/fastfloat/fast_float/archive/refs/tags/v$VERSION.tar.gz"
SRC_SHA=4bfabb5979716995090ce68dce83f88f99629bc17ae280eae79311c5340143e1

fetch "$SRC_URL" "$SRC_SHA" "$VENDOR/fast_float"
cmake_dep "$VENDOR/fast_float"

echo "fast-float $VERSION installed into $PREFIX"
