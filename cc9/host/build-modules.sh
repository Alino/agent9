#!/bin/bash
# build-modules.sh — build the C++23 `std` and `std.compat` module BMIs + objects
# for the cc9 target, so `import std;` / `import std.compat;` work on 9front.
#
# Outputs (under cc9/lib/modules/): std.pcm, std.compat.pcm (binary module
# interfaces) and std.pcm.o, std.compat.pcm.o (their codegen, linked into module
# programs). A test that does `import std;` compiles with
#   -fmodule-file=std=<lib/modules/std.pcm>
# and links std.pcm.o + the cc9 archives (see buildone.sh's module branch).
#
# The libc++ module sources are templates configured by CMake; here we generate
# std.cppm / std.compat.cppm by hand: the .in files #include every standard
# header in a global module fragment, then splice the per-header export
# fragments (modules/std/*.inc) in place of the @LIBCXX_MODULE_STD_*@ placeholder.
set -e
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
CC9="$(cd "$(dirname "$0")/.." && pwd)"
LLVMSRC="${CC9_LLVMSRC:-$HOME/Projects/llvm-project}"
LIBCXX="${CC9_LIBCXX:-/tmp/libcxx-thr/include/c++/v1}"
INC="$CC9/runtime/include"
MODSRC="$LLVMSRC/libcxx/modules"
O="$CC9/lib/modules"; mkdir -p "$O"

# Same target/define set as build-runtime.sh's libc++ objects.
B=(--target=x86_64-unknown-none -nostdlib -fexceptions -frtti -funwind-tables -fno-pic
   -nostdinc++ -femulated-tls -isystem "$LIBCXX" -isystem "$INC" -I "$MODSRC" -std=c++23 -w
   -D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE -D_LIBCPP_HAS_CLOCK_GETTIME
   -D_LIBCPP_ENABLE_CXX17_REMOVED_FEATURES -D_LIBCPP_ENABLE_CXX20_REMOVED_FEATURES
   -D_LIBCPP_ENABLE_EXPERIMENTAL)

gen() {  # gen <in> <inc-dir> <placeholder> <out>
  ls "$2"/*.inc | sed 's|^|#include "|; s|$|"|' > "$O/.incs"
  awk -v ph="$3" '$0 ~ ph {while((getline l < "'"$O/.incs"'")>0) print l; next} {gsub(/@[A-Z_]+@/,""); print}' "$1" > "$4"
  rm -f "$O/.incs"
}

echo "generating + precompiling std..."
gen "$MODSRC/std.cppm.in" "$MODSRC/std" "@LIBCXX_MODULE_STD_INCLUDE_SOURCES@" "$O/std.cppm"
"$LLVM/clang++" "${B[@]}" --precompile -o "$O/std.pcm" "$O/std.cppm"
"$LLVM/clang++" "${B[@]}" -c "$O/std.pcm" -o "$O/std.pcm.o"

echo "generating + precompiling std.compat..."
gen "$MODSRC/std.compat.cppm.in" "$MODSRC/std.compat" "@LIBCXX_MODULE_STD_COMPAT_INCLUDE_SOURCES@" "$O/std.compat.cppm"
"$LLVM/clang++" "${B[@]}" -fmodule-file=std="$O/std.pcm" --precompile -o "$O/std.compat.pcm" "$O/std.compat.cppm"
"$LLVM/clang++" "${B[@]}" -fmodule-file=std="$O/std.pcm" -c "$O/std.compat.pcm" -o "$O/std.compat.pcm.o"

echo "built module BMIs in $O:"
ls -la "$O"/*.pcm "$O"/*.pcm.o | awk '{print "  ", $5, $9}'
