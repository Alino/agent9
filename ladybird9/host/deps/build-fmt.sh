#!/bin/sh
# fmt — pinned to Ladybird's vcpkg override (12.1.0). CMake CONFIG package:
# Ladybird does find_package(fmt CONFIG REQUIRED) -> fmt::fmt.
. "$(dirname "$0")/common.sh"

VERSION=12.1.0
SRC_URL="https://github.com/fmtlib/fmt/archive/refs/tags/$VERSION.tar.gz"
SRC_SHA=ea7de4299689e12b6dddd392f9896f08fb0777ac7168897a244a6d6085043fea

fetch "$SRC_URL" "$SRC_SHA" "$VENDOR/fmt"
cmake_dep "$VENDOR/fmt" -DFMT_TEST=OFF -DFMT_DOC=OFF

echo "fmt $VERSION installed into $PREFIX"
