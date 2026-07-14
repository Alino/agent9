#!/bin/sh
# apply-patches.sh — apply llvm9's LLVM patches to $CC9_LLVMSRC (llvm-project
# lives OUTSIDE this repo, so its changes must be tracked here or a fresh clone
# silently loses them). Idempotent: skips already-applied. Run after cloning
# llvm-project and before host/build-llvm9.py.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)                 # llvm9/host
LLVMSRC="${CC9_LLVMSRC:-$HOME/Projects/llvm-project}"
[ -d "$LLVMSRC/llvm" ] || { echo "no llvm-project at $LLVMSRC"; exit 1; }

for p in "$HERE"/../patches/*.patch; do
	[ -e "$p" ] || continue
	if patch -d "$LLVMSRC" -p1 --dry-run -R -f <"$p" >/dev/null 2>&1; then
		echo "already applied: $(basename "$p")"
	else
		echo "applying: $(basename "$p")"
		patch -d "$LLVMSRC" -p1 <"$p"
	fi
done
echo "patches applied."
