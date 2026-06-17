#!/bin/sh
# Apply the Plan 9 CPython source patches to a pristine vendored tree.
# Run from anywhere: parity/fetch_cpython.sh first, then this.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../../../cpython/src"
[ -d "$SRC" ] || { echo "missing $SRC -- run parity/fetch_cpython.sh first" >&2; exit 1; }
( cd "$SRC" && patch -p1 -N < "$HERE/plan9-cpython.patch" )
echo "patches applied. Next: copy ../pyconfig.h to $SRC/pyconfig.h and put ../ape-shim on the include path (see ../README.md)."
