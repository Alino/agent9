#!/bin/bash
# build-clang.sh — G2: cross-build a reduced clang to 9front amd64 with cc9.
# Reuses the LLVM build dir from build-llvm-support.sh (reconfigures it to add
# the clang project) so the already-cross-compiled Support/etc. objects are kept.
# Host clang-tblgen must match the source tree (checked in build-llvm-support.sh).
set -euo pipefail
CC9="$(cd "$(dirname "$0")/.." && pwd)"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
BUILD="${CC9_LLVM_BUILD:-/tmp/cc9-llvm}"

[ -f "$BUILD/build.ninja" ] || { echo "run build-llvm-support.sh first ($BUILD missing)"; exit 1; }

# Reconfigure in place: add clang, point clang-tblgen at the host, trim clang.
cmake "$BUILD" \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DCLANG_TABLEGEN="$LLVM/clang-tblgen" \
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
  -DCLANG_ENABLE_ARCMT=OFF \
  -DCLANG_BUILD_TOOLS=ON \
  -DCLANG_PLUGIN_SUPPORT=OFF \
  -DHAVE_GETPAGESIZE=1

echo "configured. Build clang with:  ninja -C $BUILD clang"
