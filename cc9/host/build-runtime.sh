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
# libc++ headers built with localization+monotonic-clock ON (the iostream/regex
# tree). _LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE: use libc++'s own ctype table on
# this minimal platform (no BSD <ctype.h> rune masks).
LIBCXX="${CC9_LIBCXX:-/tmp/libcxx-thr/include/c++/v1}"
LLVMSRC="${CC9_LLVMSRC:-$HOME/Projects/llvm-project}"
INC="$CC9/runtime/include"
O="/tmp/cc9-rt"; rm -rf "$O"; mkdir -p "$O" "$CC9/lib"  # clean: stale .o would re-enter the archive via the *.o glob

# NB: no -ffreestanding — must match the user-code compile (cc9 wrapper drops it
# for the `main` symbol), else std::string's out-of-line ABI mismatches the
# header-instantiated one and over-SSO strings corrupt.
base=(--target=x86_64-unknown-none -nostdlib -fno-exceptions -frtti -nostdinc++ -D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE -D_LIBCPP_HAS_CLOCK_GETTIME -femulated-tls)

"$LLVM/clang" -target x86_64-unknown-none -c "$CC9/test/n9syscall.s" -o "$O/n9syscall.o"
"$LLVM/clang" -target x86_64-unknown-none -c "$CC9/runtime/setjmp.s" -o "$O/setjmp.o"
"$LLVM/clang" "${base[@]}" -isystem "$INC" -fno-builtin -c "$CC9/runtime/n9libc.c" -o "$O/n9libc.o"
"$LLVM/clang" "${base[@]}" -isystem "$INC" -O2 -c "$CC9/runtime/complex_builtins.c" -o "$O/complex_builtins.o"
"$LLVM/clang" "${base[@]}" -isystem "$INC" -fno-builtin -c "$CC9/runtime/xlocale.c" -o "$O/xlocale.o"
"$LLVM/clang" "${base[@]}" -isystem "$INC" -fno-builtin -c "$CC9/runtime/stdio.c" -o "$O/stdio.o"
"$LLVM/clang" "${base[@]}" -isystem "$INC" -fno-builtin -c "$CC9/runtime/printf.c" -o "$O/printf.o"
"$LLVM/clang" "${base[@]}" -isystem "$INC" -fno-builtin -c "$CC9/runtime/pthread.c" -o "$O/pthread.o"
"$LLVM/clang" "${base[@]}" -isystem "$INC" -fno-builtin -c "$CC9/runtime/wchar.c" -o "$O/wchar.o"
"$LLVM/clang" "${base[@]}" -isystem "$INC" -fno-builtin -c "$CC9/runtime/fs.c" -o "$O/fs.o"
"$LLVM/clang" "${base[@]}" -isystem "$INC" -O2 -c "$CC9/runtime/int128.c" -o "$O/int128.o"
"$LLVM/clang++" "${base[@]}" -std=c++23 -isystem "$LIBCXX" -isystem "$INC" -c "$CC9/runtime/cxxrt.cpp" -o "$O/cxxrt.o"
"$LLVM/clang++" "${base[@]}" -std=c++23 -isystem "$LIBCXX" -isystem "$INC" -c "$CC9/runtime/typeinfo_min.cpp" -o "$O/typeinfo_min.o"
"$LLVM/clang" "${base[@]}" -c "$CC9/runtime/crt0.c" -o "$O/crt0.o"

# targeted libc++ runtime objects
lcxx=("${base[@]}" -D_LIBCPP_BUILDING_LIBRARY -D_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER
      -I "$LLVMSRC/libcxx/src" -I "$LIBCXX" -isystem "$INC" -std=c++23 -DNDEBUG -O0 -w)
for f in string stdexcept memory hash functional bind memory_resource system_error error_category valarray chrono expected exception locale ios iostream ostream regex thread mutex mutex_destructor condition_variable condition_variable_destructor shared_mutex future atomic barrier; do
  "$LLVM/clang++" "${lcxx[@]}" -c "$LLVMSRC/libcxx/src/$f.cpp" -o "$O/lcx_$f.o"
done
"$LLVM/clang++" "${lcxx[@]}" -c "$LLVMSRC/libcxx/src/algorithm.cpp" -o "$O/lcx_algorithm.o"
# float std::to_chars shim (integer to_chars is header-inline; charconv.cpp's
# from_chars float side needs shared/fp_bits.h, absent from this checkout).
"$LLVM/clang++" "${lcxx[@]}" -c "$CC9/runtime/tochars.cpp" -o "$O/tochars.o"
"$LLVM/clang++" "${lcxx[@]}" -D_LIBCPP_USING_DEV_RANDOM -c "$LLVMSRC/libcxx/src/random.cpp" -o "$O/lcx_random.o"
for r in d2s f2s d2fixed; do
  "$LLVM/clang++" "${lcxx[@]}" -c "$LLVMSRC/libcxx/src/ryu/$r.cpp" -o "$O/lcx_ryu_$r.o"
done
"$LLVM/clang++" "${lcxx[@]}" -c "$LLVMSRC/libcxx/src/filesystem/filesystem_clock.cpp" -o "$O/lcx_fsclock.o"
for ff in operations directory_iterator directory_entry path filesystem_error; do
  "$LLVM/clang++" "${lcxx[@]}" -c "$LLVMSRC/libcxx/src/filesystem/$ff.cpp" -o "$O/lcx_fs_$ff.o"
done
"$LLVM/clang++" "${lcxx[@]}" -c "$LLVMSRC/libcxx/src/ios.instantiations.cpp" -o "$O/lcx_iosinst.o"
"$LLVM/clang++" "${lcxx[@]}" -c "$LLVMSRC/libcxx/src/call_once.cpp" -o "$O/lcx_callonce.o"
# libcxxabi RTTI runtime: __dynamic_cast + the __class_type_info vtables.
"$LLVM/clang++" "${base[@]}" -D_LIBCXXABI_BUILDING_LIBRARY -I "$LLVMSRC/libcxxabi/include" \
  -I "$LLVMSRC/libcxxabi/src" -I "$LIBCXX" -isystem "$INC" -std=c++23 -DNDEBUG -O0 -w \
  -c "$LLVMSRC/libcxxabi/src/private_typeinfo.cpp" -o "$O/abi_typeinfo.o"
# NB: libcxx/src/exception.cpp (in the loop above) is a strict superset of
# libcxxabi's stdlib_exception.cpp — it provides the exception/bad_alloc/
# bad_exception/bad_array_new_length destructors AND bad_typeid/bad_cast/
# nested_exception/exception_ptr/terminate — so we do NOT also link
# stdlib_exception.cpp (that would duplicate ~exception()).

# rm first: `ar rcs` only inserts/replaces, never removes — a dropped object
# would otherwise linger in the archive across rebuilds.
rm -f "$CC9/lib/libcc9cxx.a"
"$LLVM/llvm-ar" rcs "$CC9/lib/libcc9cxx.a" "$O"/*.o
echo "built $CC9/lib/libcc9cxx.a ($("$LLVM/llvm-ar" t "$CC9/lib/libcc9cxx.a" | wc -l | tr -d ' ') objects)"
