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
#   15-18  the NATIVE (compiler-on-9front) build via CBE+cc9 (see native/). 15 lib
#          (start.zig/x86_64.zig, ofmt==c-guarded), 16 lib (plan9 fs/process arms +
#          getrandom + rename), 17 src (single-threaded gpa in main.zig), 18 src
#          (Compilation.zig cross-dir cache move). Enough for `build-exe`/`run`/
#          `test` natively — the released `pac9 install zig9` binary.
#
# NOT YET EXTRACTED TO PATCHES (live in the working vendor/zig tree; see NOTES.md
# "zig build natively"): the Phase-6 `zig build` runtime — Child.zig spawnPlan9,
# io.zig pollPlan9, Progress .plan9, main.zig cmdBuild runner-defaults +
# Cc9Allocator, Package/Fetch.zig cross-dir rename, link/Plan9.zig seeNav
# got_index, arch/x86_64/CodeGen.zig getOffset .memory. These build the released
# binary but `zig build` is still blocked by a latent heap-OOB at build_runner
# scale, so the set is WIP; it'll be patch-ified once that's pinned. The getOffset
# / Cc9Allocator / seeNav pieces are additive robustness improvements validated
# correct by the two bit-exact heavy demos.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)              # zig9/port/plan9
ZIG_SRC="${ZIG_SRC:-$HERE/../../vendor/zig}"
[ -d "$ZIG_SRC/lib/std" ] || { echo "no Zig source at $ZIG_SRC (run zig9/fetch.sh)"; exit 1; }
for p in "$HERE"/patches/*.patch; do
	echo "applying $(basename "$p")"
	patch -p1 -d "$ZIG_SRC" < "$p"
done
echo "done. build with: zig9/host/zig9 build SRC"
