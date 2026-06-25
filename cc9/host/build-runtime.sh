#!/bin/bash
# build-runtime.sh — build the cc9 C++ runtime archive: libcc9cxx.a
#
# Bundles: the Plan 9 syscall thunks, the minimal freestanding libc shim
# (n9libc), the C++ runtime (cxxrt), and the targeted libc++/libc++abi runtime
# objects compiled from source for the x86_64->Plan9 target (string, stdexcept,
# memory, std::exception base) — NOT the whole library.
#
# Prereqs (build libc++ headers once; see docs/plans/*llvm* recipe):
#   - brew llvm + lld
#   - llvm-project sources (sparse: libcxx libcxxabi) at $CC9_LLVMSRC
#   - the freestanding libc++ header tree at $CC9_LIBCXX
set -euo pipefail
CC9="$(cd "$(dirname "$0")/.." && pwd)"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
LIBCXX="${CC9_LIBCXX:-/tmp/libcxx-build/include/c++/v1}"
LLVMSRC="${CC9_LLVMSRC:-$HOME/Projects/llvm-project}"
INC="$CC9/runtime/include"
O="/tmp/cc9-rt"; mkdir -p "$O" "$CC9/lib"

base=(--target=x86_64-unknown-none -ffreestanding -nostdlib -fno-exceptions -fno-rtti -nostdinc++)

"$LLVM/clang" -target x86_64-unknown-none -c "$CC9/test/n9syscall.s" -o "$O/n9syscall.o"
"$LLVM/clang" "${base[@]}" -isystem "$INC" -fno-builtin -c "$CC9/runtime/n9libc.c" -o "$O/n9libc.o"
"$LLVM/clang++" "${base[@]}" -c "$CC9/runtime/cxxrt.cpp" -o "$O/cxxrt.o"
"$LLVM/clang" "${base[@]}" -c "$CC9/runtime/crt0.c" -o "$O/crt0.o"

# targeted libc++ runtime objects
lcxx=("${base[@]}" -D_LIBCPP_BUILDING_LIBRARY -D_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER
      -I "$LLVMSRC/libcxx/src" -I "$LIBCXX" -isystem "$INC" -std=c++23 -O2 -w)
for f in string stdexcept memory hash functional; do
  "$LLVM/clang++" "${lcxx[@]}" -c "$LLVMSRC/libcxx/src/$f.cpp" -o "$O/lcx_$f.o"
done
"$LLVM/clang++" "${lcxx[@]}" -c "$LLVMSRC/libcxx/src/algorithm.cpp" -o "$O/lcx_algorithm.o"
"$LLVM/clang++" "${base[@]}" -D_LIBCXXABI_BUILDING_LIBRARY \
  -I "$LLVMSRC/libcxxabi/include" -I "$LLVMSRC/libcxxabi/src" -I "$LIBCXX" \
  -isystem "$INC" -std=c++23 -O2 -w \
  -c "$LLVMSRC/libcxxabi/src/stdlib_exception.cpp" -o "$O/abi_exc.o"

"$LLVM/llvm-ar" rcs "$CC9/lib/libcc9cxx.a" "$O"/*.o
echo "built $CC9/lib/libcc9cxx.a ($("$LLVM/llvm-ar" t "$CC9/lib/libcc9cxx.a" | wc -l | tr -d ' ') objects)"
