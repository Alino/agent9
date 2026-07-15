#!/bin/bash
# build-ladybird.sh — cross-configure + build the pinned Ladybird for 9front.
#   host/deps recipes must have provisioned _out/deps first (ICU, skia, ...).
#   Usage: build-ladybird.sh [ninja targets...]   (default: js)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LB9="$(cd "$HERE/.." && pwd)"
SRC="$LB9/vendor/ladybird"
BUILD="$LB9/_out/build"

# Rust: the rust-src-full stage1 rustc carries x86_64-unknown-plan9 built-in
# with a prebuilt plan9 std (link once: rustup toolchain link plan9 \
#   ~/Projects/rust-src-full/build/aarch64-apple-darwin/stage1).
export RUSTUP_TOOLCHAIN=plan9
# cc for cc-rs build scripts targeting plan9:
export CC_x86_64_unknown_plan9="$(cd "$HERE/../.." && pwd)/servo9/host/cc9-cc"

mkdir -p "$BUILD"
cmake -G Ninja -S "$SRC" -B "$BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release
ninja -C "$BUILD" "${@:-js}"

# Plan 9 a.outs from the SysV ELFs (real executables only).
for exe in "${@:-js}"; do
  elf="$BUILD/bin/$exe"
  [ -f "$elf" ] && python3 "$(cd "$HERE/../.." && pwd)/cc9/host/elf2aout.py" "$elf" "$BUILD/bin/$exe.aout"
done
