#!/bin/bash
# snapshot.sh — copy the plan9 std backends FROM the nightly rust-src INTO the
# repo overlay (the inverse of apply.sh). Run after editing the sysroot std to
# capture the port durably. Keep FILES in sync with the plan9 arms we add.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# Default: the rustup nightly sysroot rust-src. The full rustc checkout
# (~/Projects/rust-src-full/library) is the usual live tree now — pass it via
# RUST_SRC when the edits were made there.
RS="${RUST_SRC:-$(rustc +nightly --print sysroot)/lib/rustlib/src/rust/library}"
OV="$HERE/overlay/library"

FILES=(
  std/build.rs
  std/src/sys/exit.rs
  std/src/sys/pal/mod.rs
  std/src/sys/pal/plan9/mod.rs
  std/src/sys/pal/plan9/sync/mod.rs
  std/src/sys/pal/plan9/sync/mutex.rs
  std/src/sys/pal/plan9/sync/condvar.rs
  std/src/sys/alloc/mod.rs std/src/sys/alloc/plan9.rs
  std/src/sys/random/mod.rs std/src/sys/random/plan9.rs
  std/src/sys/stdio/mod.rs std/src/sys/stdio/plan9.rs
  std/src/sys/thread_local/mod.rs std/src/sys/thread_local/key/plan9.rs
  std/src/sys/io/error/mod.rs std/src/sys/io/error/plan9.rs
  std/src/sys/args/mod.rs std/src/sys/args/plan9.rs
  std/src/sys/env/mod.rs std/src/sys/env/plan9.rs
  std/src/sys/time/mod.rs std/src/sys/time/plan9.rs
  std/src/sys/thread/mod.rs std/src/sys/thread/plan9.rs
  std/src/sys/fs/mod.rs std/src/sys/fs/plan9.rs
  std/src/sys/net/connection/mod.rs std/src/sys/net/connection/plan9.rs
  std/src/sys/paths/mod.rs std/src/sys/paths/plan9.rs
  std/src/sys/sync/once/mod.rs
  std/src/sys/sync/mutex/mod.rs
  std/src/sys/sync/condvar/mod.rs
  std/src/sys/sync/rwlock/mod.rs
  std/src/sys/sync/thread_parking/mod.rs
  std/src/sys/personality/mod.rs
  std/src/sys/process/mod.rs
  std/src/sys/process/plan9.rs
  std/src/sys/pipe/mod.rs
  std/src/sys/pipe/plan9.rs
  unwind/src/lib.rs
  panic_unwind/src/lib.rs
)

rm -rf "$OV"
for f in "${FILES[@]}"; do
  mkdir -p "$OV/$(dirname "$f")"
  cp "$RS/$f" "$OV/$f"
done
rustc +nightly --version > "$HERE/NIGHTLY-PIN.txt"
echo "snapshotted ${#FILES[@]} files -> $OV"
