#!/bin/bash
# build-libm.sh — build cc9/lib/libcc9m.a, a freestanding libm for 9front amd64
# cross-compiled from openlibm (JuliaMath/openlibm, ISC/BSD). Gives cc9 a real,
# correctly-rounded libm with full special-case (inf/nan/signbit) semantics and
# 80-bit long double — what <complex>/<cmath>/numerics need.
#
# Static archive: a program pulls only the math objects it references, so the
# C99 _Complex files (cabs/csqrt, need compiler-rt __mulsc3) never link unless
# actually used; std::complex<T> is a template over the REAL libm and doesn't.
#
# Config (env): CC9_LLVM, CC9_OPENLIBM (openlibm checkout).
set -euo pipefail
CC9="$(cd "$(dirname "$0")/.." && pwd)"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
OL="${CC9_OPENLIBM:-$HOME/Projects/openlibm}"
INC="$CC9/runtime/include"
O=/tmp/cc9-libm; rm -rf "$O"; mkdir -p "$O" "$CC9/lib"
[ -d "$OL/src" ] || { echo "openlibm not found at $OL (set CC9_OPENLIBM)"; exit 1; }

inc=(-isystem "$INC" -I "$OL" -I "$OL/src" -I "$OL/include" -I "$OL/ld80")
cflags=(--target=x86_64-unknown-none -nostdlib "${inc[@]}" -D__BSD_VISIBLE -O2 -w)

# These ld80 files are OpenBSD-style (need machine/ieee.h, struct ieee_ext);
# the FreeBSD-style src/ versions implement the same long double functions and
# compile cleanly, so keep src/ for them.
ld80_skip=" e_fmodl s_remquol s_truncl s_nexttoward s_nexttowardf "

# 1) all of src/
for f in "$OL"/src/*.c; do
  "$LLVM/clang" "${cflags[@]}" -c "$f" -o "$O/$(basename "$f" .c).o"
done
# 2) overlay ld80/ (80-bit long double) — overrides the generic src/ versions,
#    except the OpenBSD-style stragglers above.
for f in "$OL"/ld80/*.c; do
  b=$(basename "$f" .c)
  case "$ld80_skip" in *" $b "*) continue;; esac
  "$LLVM/clang" "${cflags[@]}" -c "$f" -o "$O/$b.o"
done

rm -f "$CC9/lib/libcc9m.a"
"$LLVM/llvm-ar" rcs "$CC9/lib/libcc9m.a" "$O"/*.o
echo "built $CC9/lib/libcc9m.a ($("$LLVM/llvm-ar" t "$CC9/lib/libcc9m.a" | wc -l | tr -d ' ') objects)"
