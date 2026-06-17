#!/bin/sh
# Build the reference CPython 3.11.14 on the build host (macOS/Linux).
# Produces an in-tree interpreter with the full Lib/test suite, which is the
# oracle the 9front port is scored against.
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)   # python9/
SRC="$HERE/cpython/src"
PREFIX="$HERE/cpython/host-build"

[ -d "$SRC" ] || { echo "missing $SRC -- run fetch_cpython.sh first" >&2; exit 1; }

cd "$SRC"
if [ ! -f Makefile ]; then
	# Optimizations off on purpose: fast, deterministic, and identical code
	# paths to what we'll port. We are measuring correctness, not speed.
	./configure --prefix="$PREFIX" --with-ensurepip=no >/tmp/py-configure.log 2>&1
fi
# The in-tree ./python(.exe) is all the harness needs; no install required.
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/tmp/py-make.log 2>&1

# macOS builds the binary as python.exe (and ./python is the case-insensitive
# match for the Python/ source dir, so check python.exe first); Linux: ./python.
if [ -f ./python.exe ]; then BIN=./python.exe; else BIN=./python; fi
echo "built reference interpreter: $SRC/$BIN"
"$SRC/$BIN" --version
