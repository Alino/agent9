#!/bin/bash
# build-lld.sh — G4: cross-build ld.lld (LLVM's ELF linker) to 9front amd64 with
# cc9, mirroring build-clang.sh. Reuses the LLVM build dir from
# build-llvm-support.sh + build-clang.sh (reconfigures in place to add the lld
# project) so the already-cross-compiled LLVM libs (Support, MC, Object, LTO,
# X86*, …) are kept and only lldCommon/lldELF/the lld tool get built.
#
# Output: $BUILD/bin/lld — a Plan 9 amd64 a.out (cc9-link already runs elf2aout
# on the linked ELF). On 9front invoke it as `ld.lld` (the binary dispatches on
# argv[0]) or `lld -flavor gnu`.
#
# !! ALWAYS pass --mmap-output-file on 9front (see the note at the bottom) or the
#    linker silently writes a zero-filled output file. !!
set -euo pipefail
CC9="$(cd "$(dirname "$0")/.." && pwd)"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
BUILD="${CC9_LLVM_BUILD:-/tmp/cc9-llvm}"

[ -f "$BUILD/build.ninja" ] || { echo "run build-llvm-support.sh then build-clang.sh first ($BUILD missing)"; exit 1; }

# Reconfigure in place: add lld next to clang. Threads stay OFF (inherited from
# the support build's cache) so lld's parallel passes (Writer, ICF) run serially
# — no extra flag needed. lld has no private tablegen: ELF/Options.td is generated
# with the host llvm-tblgen already pinned in the cache. CLANG_TABLEGEN is
# re-passed only so a build dir that somehow lost it can still re-add clang.
cmake "$BUILD" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DCLANG_TABLEGEN="$LLVM/clang-tblgen" \
  -DLLD_BUILD_TOOLS=ON

echo "configured. Build the linker with:"
echo "  ninja -C $BUILD lld     # -> $BUILD/bin/lld  (Plan 9 a.out)"
echo
echo "G4 on-box chain (all on 9front):"
echo "  clang -c foo.c -o foo.o"
echo "  ld.lld --mmap-output-file -static -nostdlib -T plan9.ld \\"
echo "         --start-group foo.o libcc9cxx.a libcc9m.a --end-group -o foo.elf"
echo "  elf2aout foo.elf foo          # python9 elf2aout.py, or a C port (gap)"
echo
echo "WHY --mmap-output-file (load-bearing):"
echo "  Without it lld uses FileOutputBuffer::createOnDiskBuffer — a temp file"
echo "  mmap'd read-write, with the data flushed by munmap on commit(). cc9 mmap"
echo "  is malloc+pread and munmap=free, so the dirty pages are dropped and the"
echo "  output is zero-filled. --mmap-output-file selects createInMemoryBuffer:"
echo "  an anonymous malloc buffer plus an explicit raw_fd_ostream write() on"
echo "  commit() — the exact path clang already uses for .o files, so it persists."
echo "  No pwrite write-back shim in cc9 mmap is required."
