#!/bin/bash
# regen-libcxx.sh — regenerate the freestanding libc++ header tree at
# /tmp/libcxx-thr (macOS periodically wipes /tmp, taking the tree with it —
# symptom: build-runtime.sh dies on `#include <new>`). Source of truth:
# ~/Projects/llvm-project @ llvmorg-22.1.8. Config = the proven "thr" tree
# (localization+threads+monotonic+unicode+wide-chars+filesystem ON,
# exceptions/RTTI default-ON), then the two hand-patches to __config_site:
# THREAD_API_PTHREAD=1 (cc9 pthread over rfork), HAS_TERMINAL=0.
set -euo pipefail
LLVMPROJ="${CC9_LLVMPROJ:-$HOME/Projects/llvm-project}"
TREE="${CC9_LIBCXX_TREE:-/tmp/libcxx-thr}"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"

rm -rf "$TREE"
cmake -G Ninja -S "$LLVMPROJ/runtimes" -B "$TREE" \
  -DLLVM_ENABLE_RUNTIMES=libcxx -DLIBCXX_CXX_ABI=none \
  -DCMAKE_C_COMPILER="$LLVM/clang" -DCMAKE_CXX_COMPILER="$LLVM/clang++" \
  -DCMAKE_C_COMPILER_TARGET=x86_64-unknown-none \
  -DCMAKE_CXX_COMPILER_TARGET=x86_64-unknown-none \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DLIBCXX_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF \
  -DLIBCXX_ENABLE_LOCALIZATION=ON -DLIBCXX_ENABLE_MONOTONIC_CLOCK=ON \
  -DLIBCXX_ENABLE_UNICODE=ON -DLIBCXX_ENABLE_THREADS=ON \
  -DLIBCXX_ENABLE_WIDE_CHARACTERS=ON -DLIBCXX_ENABLE_FILESYSTEM=ON \
  > /tmp/libcxx-cmake.log
ninja -C "$TREE" generate-cxx-headers >/dev/null
CS="$TREE/include/c++/v1/__config_site"
sed -i '' 's/#define _LIBCPP_HAS_TERMINAL 1/#define _LIBCPP_HAS_TERMINAL 0/; s/#define _LIBCPP_HAS_THREAD_API_PTHREAD 0/#define _LIBCPP_HAS_THREAD_API_PTHREAD 1/' "$CS"
grep -q "_LIBCPP_HAS_THREAD_API_PTHREAD 1" "$CS"
echo "regenerated $TREE (verify with: cc9/host/build-runtime.sh)"
