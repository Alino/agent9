#!/bin/bash
# apply.sh — install the plan9 std backends into the nightly rust-src tree.
#
# The M3 std port (std::sys::pal::plan9 + the per-feature plan9 backends, wired
# into the cfg_select dispatchers) lives as an OVERLAY under std/overlay/library.
# It is version-pinned: the dispatcher mod.rs files are whole-file copies, so they
# must match the pinned nightly (std/NIGHTLY-PIN.txt). On a matching toolchain this
# just copies the overlay over rust-src.
#
# After applying (or any edit to the overlay), `cargo clean` in the build dir —
# cargo's -Zbuild-std does NOT detect edits to sysroot rust-src sources.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
RS="$(rustc +nightly --print sysroot)/lib/rustlib/src/rust/library"
[ -d "$RS" ] || { echo "rust-src not found; run: rustup component add rust-src --toolchain nightly"; exit 1; }

PIN="$(cat "$HERE/NIGHTLY-PIN.txt")"
NOW="$(rustc +nightly --version)"
if [ "$PIN" != "$NOW" ]; then
  echo "WARNING: nightly mismatch — overlay pinned to:"
  echo "  $PIN"
  echo "  but active nightly is: $NOW"
  echo "The plan9 backend files still apply, but the whole-file dispatcher copies"
  echo "(mod.rs) may clobber upstream changes. Re-snapshot if the build breaks."
fi

cp -R "$HERE/overlay/library/." "$RS/"
echo "applied plan9 std overlay -> $RS"
echo "NOTE: run 'cargo clean' before rebuilding (build-std caches sysroot std)."
