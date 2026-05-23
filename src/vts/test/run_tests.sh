#!/bin/bash
# run_tests.sh — build and run vts unit tests on the host.
#
# These tests cover the portable C parts of vts (cell buffer, VT100 parser).
# Plan 9-specific pieces (lib9p, libthread, libdraw) are tested in the VM.

set -euo pipefail

cd "$(dirname "$0")/.."

CC=${CC:-clang}
CFLAGS="-DHOST -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function -g -O0"

mkdir -p test/bin

compile() {
    local target="$1"
    shift
    echo ">> build test/bin/$target"
    "$CC" $CFLAGS -o "test/bin/$target" "$@"
}

run() {
    local target="$1"
    echo ">> run test/bin/$target"
    "./test/bin/$target"
}

# test_cells
compile test_cells test/test_cells.c cells.c

# test_lined
compile test_lined test/test_lined.c lined.c

# test_celldiff
compile test_celldiff test/test_celldiff.c celldiff.c cells.c

# Parser tests come later (after Phase 2.2/2.3)
if [ -f test/test_parser.c ]; then
    compile test_parser test/test_parser.c parser.c cells.c
fi

# Run them
run test_cells
run test_celldiff
run test_lined
if [ -f test/bin/test_parser ]; then
    run test_parser
fi

echo
echo "✓ all tests passed"
