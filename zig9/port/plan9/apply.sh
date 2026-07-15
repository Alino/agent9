#!/bin/sh
# Apply the zig9 plan9 std-library patches to the vendored Zig source tree.
# Idempotent-ish: re-running on an already-patched tree will report failed hunks.
#
#   sh port/plan9/apply.sh            # patch zig9/vendor/zig
#   ZIG_SRC=/path/to/zig sh port/plan9/apply.sh
#
# Clean, upstreamable fixes to Zig 0.14.1's experimental x86_64-plan9 target —
# see NOTES.md / README.md for the why of each.
#   01-03,05,07,09,11  lib/std only -> picked up via `zig --zig-lib-dir <tree>/lib`
#                               with the prebuilt 0.14.1 host toolchain (no rebuild).
#   04,06,08,10,12,13,14  src/ (backend) -> require REBUILDING the compiler (see linux-build.sh);
#                           the prebuilt host binary will not have it.
#   15-17  the NATIVE (compiler-on-9front) build via CBE+cc9 (see native/). 15 lib
#          (start.zig/x86_64.zig, ofmt==c-guarded), 16 lib (plan9 fs/process arms +
#          getrandom + rename), 17 src (single-threaded gpa in main.zig).
#   18-22  the native `zig build` set (see NOTES.md "zig build natively — LANDED"
#          and the GC/heap-corruption section): 18 cross-dir cache moves
#          (Compilation + Package/Fetch), 19 compiler-rt compiled into the zcu
#          (strong linkage, ALL float families — the linker GC sweeps unused
#          ones; only the 4 f16 helpers the backend can't compile stay gated),
#          20 the linker: named-symbol GOT resolution, GC (edge recording +
#          mark/sweep, zeroed GOT), 2MB-boundary hard error, bases init in
#          createEmpty, entry from the _start atom, deinit undefined-free fixes
#          (createAtom .code / lazy-sym names — THE years-latent heap
#          corruption), emit-fd self-heal, Lower call/jmp mem promotion,
#          21 ONE break owner (plan9.sbrk → n9libc's exported cc9_sbrk under the
#          C backend) + std.os.plan9 gaps, 22 the zig-build runtime (Child
#          spawnPlan9, io pollPlan9, Progress/Watch/build_runner gates,
#          Options/Run moves, Target vector workarounds, Cc9Allocator with real
#          free + magic-validated headers, cmdBuild runner defaults).
#          The companion cc9_sbrk export lives in cc9/runtime/n9libc.c (git-
#          tracked, not a zig patch). Whole stack verified: pristine 0.14.1 +
#          01..22 reproduces the shipping vendor tree byte-for-byte.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)              # zig9/port/plan9
ZIG_SRC="${ZIG_SRC:-$HERE/../../vendor/zig}"
[ -d "$ZIG_SRC/lib/std" ] || { echo "no Zig source at $ZIG_SRC (run zig9/fetch.sh)"; exit 1; }
for p in "$HERE"/patches/*.patch; do
	echo "applying $(basename "$p")"
	patch -p1 -d "$ZIG_SRC" < "$p"
done
echo "done. build with: zig9/host/zig9 build SRC"
