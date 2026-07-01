#!/bin/sh
# make-tarball.sh — assemble gl9-amd64.tar.gz for pac9's `tarball` install: the
# cross-compiled GL demos + the `gl9` launcher, laid out at / (amd64/bin, rc/bin,
# sys/lib). gl9win is NOT here — it's the on-box-built `gl9win` package (a dep).
# Publish the result as the GitHub release `gl9` asset.
#
# Prereq: build the demos first —
#   python3 host/build-gl9.py link test/corpus/cube_demo.c
#   python3 host/build-gl9.py link test/corpus/egl_demo.c port/plan9/egl/gl9egl.c
set -e
HERE=$(cd "$(dirname "$0")" && pwd); GL9=$(dirname "$HERE")
OUT="$GL9/_out"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
CC9="${CC9:-$(dirname "$GL9")/cc9}"
STAGE="$HERE/stage"; TARBALL="$HERE/gl9-amd64.tar.gz"

# installed-name : built-a.out-basename
demos="gl9-cube:cube_demo gl9-egl:egl_demo"

rm -rf "$STAGE"
mkdir -p "$STAGE/amd64/bin" "$STAGE/rc/bin" "$STAGE/sys/lib/gl9"

for d in $demos; do
	name=${d%%:*}; src=${d##*:}
	elf="$OUT/$src.elf"
	[ -f "$elf" ] || { echo "missing $elf — build the demo first (see header)"; exit 1; }
	# a Plan 9 a.out needs no symbol table to run: strip the ELF, then reconvert.
	"$LLVM/llvm-strip" --strip-all "$elf" -o "$STAGE/tmp.elf"
	python3 "$CC9/host/elf2aout.py" "$STAGE/tmp.elf" "$STAGE/amd64/bin/$name" >/dev/null
	chmod +x "$STAGE/amd64/bin/$name"   # elf2aout output isn't +x; the tarball must be
	rm -f "$STAGE/tmp.elf"
	echo "  $name  $(du -h "$STAGE/amd64/bin/$name" | cut -f1)"
done

cp "$HERE/gl9" "$STAGE/rc/bin/gl9"; chmod +x "$STAGE/rc/bin/gl9"
cp "$HERE/README" "$STAGE/sys/lib/gl9/README"

# ustar format so 9front's tar reads it (no pax/GNU extensions).
( cd "$STAGE" && tar --format ustar -czf "$TARBALL" amd64 rc sys )
echo "-> $TARBALL  ($(du -h "$TARBALL" | cut -f1))"
echo "publish:  gh release create gl9 '$TARBALL' -t 'gl9 — OpenGL for 9front'"
