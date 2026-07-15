#!/bin/sh
# simdutf — pinned to Ladybird's vcpkg override (7.4.0). CMake package:
# find_package(simdutf REQUIRED) resolves the installed simdutf-config.cmake
# (no builtin CMake module exists) -> simdutf::simdutf.
# ISA kernels use cpuid+xgetbv runtime dispatch: AVX paths only run when the
# OS advertises state save, so 9front safely falls back to SSE.
. "$(dirname "$0")/common.sh"

VERSION=7.4.0
SRC_URL="https://github.com/simdutf/simdutf/archive/refs/tags/v$VERSION.tar.gz"
SRC_SHA=8fd729ebfd5ec56cb0395bcc176c4801e1f8a0ea834d166d52279d7b9e801283

fetch "$SRC_URL" "$SRC_SHA" "$VENDOR/simdutf"
cmake_dep "$VENDOR/simdutf" \
	-DSIMDUTF_TESTS=OFF -DSIMDUTF_TOOLS=OFF -DSIMDUTF_BENCHMARKS=OFF

echo "simdutf $VERSION installed into $PREFIX"
