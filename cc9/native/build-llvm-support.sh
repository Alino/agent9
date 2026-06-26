#!/bin/bash
# build-llvm-support.sh — G1 kill-test: cross-build LLVM's Support library (and a
# tiny tool that uses it) to 9front amd64 with the cc9 toolchain. Surfaces the
# libc/OS gap cascade on a smaller-than-clang binary before committing to clang.
#
# Reduced config: X86 only, threads OFF, no exceptions/rtti (LLVM default), no
# zlib/zstd/terminfo/libxml2/libedit, static, Release. Host tablegen comes from
# brew llvm (must match the source tree version — checked below).
set -euo pipefail
CC9="$(cd "$(dirname "$0")/.." && pwd)"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
SRC="${CC9_LLVMSRC:-$HOME/Projects/llvm-project}"
BUILD="${CC9_LLVM_BUILD:-/tmp/cc9-llvm}"

# tablegen version must match the source tree (cross-build runs host tblgen).
hv="$("$LLVM/clang" --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
sv="$(git -C "$SRC" describe --tags 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
echo "host llvm=$hv  source=$sv"
[ "$hv" = "$sv" ] || echo "WARN: host/source llvm version mismatch ($hv vs $sv)"

cmake -S "$SRC/llvm" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$CC9/native/toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD=X86 \
  -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-none \
  -DLLVM_ENABLE_THREADS=OFF \
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF \
  -DLLVM_ENABLE_EXCEPTIONS=OFF -DLLVM_ENABLE_RTTI=OFF \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_TABLEGEN="$LLVM/llvm-tblgen" \
  -DLLVM_HOST_TRIPLE=x86_64-unknown-none \
  -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF \
  -DHAVE_GETPAGESIZE=1
# ^ cc9 provides getpagesize; CMake's check_symbol_exists needs linking (blocked
#   by the STATIC_LIBRARY try-compile) so we pre-seed it. Other unprobed HAVE_*
#   stay off and LLVM falls back gracefully.

echo "configured. Build the Support lib with:"
echo "  cmake --build $BUILD --target LLVMSupport"
