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
base=(--target=x86_64-unknown-none -nostdlib -fexceptions -frtti -funwind-tables -fno-pic -nostdinc++ -D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE -D_LIBCPP_HAS_CLOCK_GETTIME -femulated-tls)
# debug builds: 2GB main stack so a huge single frame doesn't overflow before its body
# calls instrumented code (the SP-usage trigger then fires). crt0 + n9libc must agree.
[ -n "${CC9_INSTRUMENT:-}" ] && base+=(-DCC9_STACK_BYTES=1073741824)

"$LLVM/clang" -target x86_64-unknown-none -c "$CC9/test/n9syscall.s" -o "$O/n9syscall.o"
"$LLVM/clang" -target x86_64-unknown-none -c "$CC9/runtime/setjmp.s" -o "$O/setjmp.o"
INSTR="${CC9_INSTRUMENT:+-finstrument-functions}"
"$LLVM/clang" "${base[@]}" $INSTR -isystem "$INC" -fno-builtin -c "$CC9/runtime/n9libc.c" -o "$O/n9libc.o"
"$LLVM/clang" "${base[@]}" $INSTR -isystem "$INC" -fno-builtin -c "$CC9/runtime/posix_llvm.c" -o "$O/posix_llvm.o"  # POSIX surface LLVM's Unix .inc needs
"$LLVM/clang" "${base[@]}" $INSTR -isystem "$INC" -O2 -c "$CC9/runtime/complex_builtins.c" -o "$O/complex_builtins.o"
"$LLVM/clang" "${base[@]}" $INSTR -isystem "$INC" -fno-builtin -c "$CC9/runtime/xlocale.c" -o "$O/xlocale.o"
"$LLVM/clang" "${base[@]}" $INSTR -isystem "$INC" -fno-builtin -c "$CC9/runtime/stdio.c" -o "$O/stdio.o"
"$LLVM/clang" "${base[@]}" $INSTR -isystem "$INC" -fno-builtin -c "$CC9/runtime/printf.c" -o "$O/printf.o"
"$LLVM/clang" "${base[@]}" $INSTR -isystem "$INC" -fno-builtin -c "$CC9/runtime/pthread.c" -o "$O/pthread.o"
"$LLVM/clang" "${base[@]}" $INSTR -isystem "$INC" -fno-builtin -c "$CC9/runtime/wchar.c" -o "$O/wchar.o"
"$LLVM/clang" "${base[@]}" $INSTR -isystem "$INC" -fno-builtin -c "$CC9/runtime/fs.c" -o "$O/fs.o"
"$LLVM/clang" "${base[@]}" $INSTR -isystem "$INC" -fno-builtin -c "$CC9/runtime/poll.c" -o "$O/poll.o"   # poll(2)/fcntl/pipe2 readiness layer (libuv)
"$LLVM/clang" "${base[@]}" $INSTR -isystem "$INC" -fno-builtin -c "$CC9/runtime/shm9.c" -o "$O/shm9.o"   # cross-process shared memory over #g named segments (ladybird9)
"$LLVM/clang" "${base[@]}" $INSTR -isystem "$INC" -fno-builtin -c "$CC9/runtime/netcompat.c" -o "$O/netcompat.o"  # inet_* for real; resolver/dgram honest stubs
"$LLVM/clang" "${base[@]}" $INSTR -isystem "$INC" -fno-builtin -c "$CC9/runtime/net9.c" -o "$O/net9.o"  # BSD sockets over /net (dial/announce/cs)
"$LLVM/clang" "${base[@]}" $INSTR -isystem "$INC" -fno-builtin -c "$CC9/runtime/fenv.c" -o "$O/fenv.o"
"$LLVM/clang" "${base[@]}" $INSTR -isystem "$INC" -O2 -c "$CC9/runtime/int128.c" -o "$O/int128.o"
"$LLVM/clang++" "${base[@]}" $INSTR -std=c++23 -isystem "$LIBCXX" -isystem "$INC" -c "$CC9/runtime/cxxrt.cpp" -o "$O/cxxrt.o"
"$LLVM/clang" "${base[@]}" ${CC9_INSTRUMENT:+-DCC9_FAULT_FILE -DCC9_PAUSE_ATTACH} -c "$CC9/runtime/crt0.c" -o "$O/crt0.o"  # crt0 NOT instrumented (naked _start); fault->file + pause-for-acid when debugging

# targeted libc++ runtime objects
# CC9_INSTRUMENT: -finstrument-functions on the out-of-line libc++ runtime so the
# __cyg_profile hooks (n9libc.c) catch a runaway recursion living in libc++ .cpp code.
INSTR="${CC9_INSTRUMENT:+-finstrument-functions}"
lcxx=("${base[@]}" $INSTR -D_LIBCPP_BUILDING_LIBRARY -D_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER
      -I "$LLVMSRC/libcxx/src" -I "$LIBCXX" -isystem "$INC" -std=c++23 -DNDEBUG -O1 -w)
for f in string stdexcept memory hash functional bind memory_resource system_error error_category valarray chrono expected locale ios iostream ostream regex thread mutex mutex_destructor condition_variable condition_variable_destructor shared_mutex future atomic barrier strstream variant any optional; do
  "$LLVM/clang++" "${lcxx[@]}" -c "$LLVMSRC/libcxx/src/$f.cpp" -o "$O/lcx_$f.o"
done
"$LLVM/clang++" "${lcxx[@]}" -c "$LLVMSRC/libcxx/src/algorithm.cpp" -o "$O/lcx_algorithm.o"
# deprecated std::random_shuffle (__rs_default/__rs_get) — needs the removed-feature opt-in
"$LLVM/clang++" "${lcxx[@]}" -D_LIBCPP_ENABLE_CXX17_REMOVED_RANDOM_SHUFFLE -c "$LLVMSRC/libcxx/src/random_shuffle.cpp" -o "$O/lcx_random_shuffle.o"
# std::__log_hardening_failure (default hardening-assertion logger)
"$LLVM/clang++" "${lcxx[@]}" -c "$LLVMSRC/libcxx/src/experimental/log_hardening_failure.cpp" -o "$O/lcx_log_hardening.o"
# Full charconv (to_chars + from_chars, float + integer). The float from_chars
# side pulls LLVM-libc's correctly-rounded parser via shared/{fp_bits,str_to_float,
# str_to_integer}.h — materialize those trees from the (sparse) llvm checkout:
#   git -C "$LLVMSRC" sparse-checkout add libc/shared libc/src/__support libc/hdr \
#       libc/include/llvm-libc-macros libc/include/llvm-libc-types
# then -I libc makes the quoted includes resolve. Replaces the old to_chars-only
# shim (runtime/tochars.cpp) — charconv.cpp defines both sides.
"$LLVM/clang++" "${lcxx[@]}" -I "$LLVMSRC/libc" -c "$LLVMSRC/libcxx/src/charconv.cpp" -o "$O/charconv.o"
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
# --- libcxxabi exception + RTTI runtime + the DWARF unwinder (replaces libc++
# exception.cpp). DWARF (clang's well-supported x86_64 path; SJLJ codegen is
# buggy on x86_64). The bare-metal unwinder finds .eh_frame via linker symbols
# __eh_frame_start/end (no dynamic loader), so a.out works. ---
abix=("${base[@]}" $INSTR -D_LIBCXXABI_BUILDING_LIBRARY -D_LIBCPP_ENABLE_CXX17_REMOVED_UNEXPECTED_FUNCTIONS
      -I "$LLVMSRC/libcxxabi/include" -I "$LLVMSRC/libcxxabi/src" -I "$LLVMSRC/libunwind/include"
      -I "$LLVMSRC/libcxx/src" -I "$LIBCXX" -isystem "$INC" -std=c++23 -DNDEBUG -O1 -w)
for a in stdlib_exception stdlib_typeinfo private_typeinfo cxa_aux_runtime abort_message \
         cxa_exception cxa_exception_storage cxa_personality cxa_handlers cxa_default_handlers \
         cxa_vector fallback_malloc cxa_demangle; do
  "$LLVM/clang++" "${abix[@]}" -c "$LLVMSRC/libcxxabi/src/$a.cpp" -o "$O/abi_$a.o"
done
# std::exception_ptr + current/rethrow_exception + nested_exception (delegating to
# the __cxa_* primary-exception API). A focused shim — NOT libc++ exception.cpp,
# which would also redefine terminate/unexpected/bad_cast/bad_typeid and clash
# with the libcxxabi objects above. Needed by std::promise/future/call_once.
"$LLVM/clang++" "${lcxx[@]}" -I "$LLVMSRC/libcxxabi/include" -c "$CC9/runtime/exception_ptr.cpp" -o "$O/exception_ptr.o"
# libunwind DWARF core: the cursor+CFI interpreter, level-1 API, register save/restore.
uwf=(--target=x86_64-unknown-none -nostdlib -I "$LLVMSRC/libunwind/include" -I "$LLVMSRC/libunwind/src"
     -isystem "$INC" -D_LIBUNWIND_IS_NATIVE_ONLY -D_LIBUNWIND_IS_BAREMETAL -D_LIBUNWIND_SUPPORT_DWARF_UNWIND
     -DNDEBUG -O1 -w -funwind-tables -fno-pic -femulated-tls)
"$LLVM/clang++" "${uwf[@]}" -frtti -fno-exceptions -nostdinc++ -isystem "$LIBCXX" -c "$LLVMSRC/libunwind/src/libunwind.cpp" -o "$O/uw_core.o"
"$LLVM/clang" "${uwf[@]}" -c "$LLVMSRC/libunwind/src/UnwindLevel1.c" -o "$O/uw_level1.o"
# gcc-ext entry points (_Unwind_GetIPInfo/GetDataRelBase/GetTextRelBase/...):
# Rust std's panic/backtrace machinery references them (ladybird9 Rust crates).
"$LLVM/clang" "${uwf[@]}" -c "$LLVMSRC/libunwind/src/UnwindLevel1-gcc-ext.c" -o "$O/uw_gccext.o"
"$LLVM/clang" "${uwf[@]}" -c "$LLVMSRC/libunwind/src/UnwindRegistersSave.S" -o "$O/uw_save.o"
"$LLVM/clang" "${uwf[@]}" -c "$LLVMSRC/libunwind/src/UnwindRegistersRestore.S" -o "$O/uw_restore.o"

# rm first: `ar rcs` only inserts/replaces, never removes — a dropped object
# would otherwise linger in the archive across rebuilds.
rm -f "$CC9/lib/libcc9cxx.a"
"$LLVM/llvm-ar" rcs "$CC9/lib/libcc9cxx.a" "$O"/*.o
echo "built $CC9/lib/libcc9cxx.a ($("$LLVM/llvm-ar" t "$CC9/lib/libcc9cxx.a" | wc -l | tr -d ' ') objects)"
