#!/bin/sh
# simdjson — pinned to Ladybird's vcpkg override (4.2.4). CMake CONFIG package:
# find_package(simdjson CONFIG REQUIRED) -> simdjson::simdjson.
# Same cpuid runtime-dispatch story as simdutf: SSE fallback is automatic.
. "$(dirname "$0")/common.sh"

VERSION=4.2.4
SRC_URL="https://github.com/simdjson/simdjson/archive/refs/tags/v$VERSION.tar.gz"
SRC_SHA=6f942d018561a6c30838651a386a17e6e4abbfc396afd0f62740dea1810dedea

fetch "$SRC_URL" "$SRC_SHA" "$VENDOR/simdjson"
cmake_dep "$VENDOR/simdjson" -DSIMDJSON_DEVELOPER_MODE=OFF

echo "simdjson $VERSION installed into $PREFIX"
