#!/bin/bash
# configure.sh — configure LLVM (X86 only, ORC on) inside a linux/amd64 container
# and natively build LLVMSupport + LLVMMC. Two outputs we need:
#   (a) the generated headers (llvm/Config/*.h, *.inc) cc9 will -I against
#   (b) build9/compile_commands.json — the TU set + flags harvest.py reads
# Native build also proves upstream compiles clean before we point cc9 at it.
#
#   sh llvm9/host/configure.sh          # image + cmake configure + ninja Support MC
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LLVMSRC="${CC9_LLVMSRC:-$HOME/Projects/llvm-project}"
IMG=llvm9build:bookworm
PLAT=linux/amd64
BUILD=build9   # under $LLVMSRC, mounted at /work/llvm-project

docker build --platform=$PLAT -t "$IMG" -f "$HERE/Dockerfile" "$HERE"

drun() { docker run --rm --platform=$PLAT -e HOME=/tmp \
  -v "$LLVMSRC:/work/llvm-project" -w /work/llvm-project "$IMG" bash -c "$1"; }

echo "== cmake configure (X86, ORC, minimal) =="
drun "cmake -G Ninja -S llvm -B $BUILD \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD=X86 \
  -DLLVM_ENABLE_PROJECTS= \
  -DLLVM_ENABLE_RUNTIMES= \
  -DLLVM_BUILD_TOOLS=OFF \
  -DLLVM_BUILD_UTILS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF \
  -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"

echo "== native build: tablegen headers + LLVMSupport + LLVMMC =="
# -j4: emulated amd64 at -j\$(nproc) OOMs Docker's VM (gl9 lesson). Support/MC pull
# in the intrinsics + config header generation as dependencies.
drun "cd $BUILD && ninja -j4 LLVMSupport LLVMMC"

echo "== done: $LLVMSRC/$BUILD/compile_commands.json =="
