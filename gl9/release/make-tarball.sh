#!/bin/sh
# make-tarball.sh — assemble gl9-amd64.tar.gz for pac9's `tarball` install: the
# cross-compiled GL demos + the `gl9` launcher + the CHANGELOG, laid out at /
# (amd64/bin, rc/bin, sys/lib). gl9win is NOT here — it's the on-box-built
# `gl9win` package (a dep).
#
# VERSION below must match the registry's 6th column, and the release tag must
# embed it (gl9-v$VERSION) so the download URL is immutable — a tag you can
# overwrite is not a version.
#
# Prereq: build the demos first. GL9_LLVM=1 links the llvm-enabled Mesa, so the
# binaries carry BOTH rasterizers and the launcher picks at runtime:
#   GL9_LLVM=1 python3 host/build-gl9.py link test/corpus/cube_demo.c
#   GL9_LLVM=1 python3 host/build-gl9.py link test/corpus/egl_demo.c port/plan9/egl/gl9egl.c
set -e
HERE=$(cd "$(dirname "$0")" && pwd); GL9=$(dirname "$HERE")
OUT="$GL9/_out"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
CC9="${CC9:-$(dirname "$GL9")/cc9}"
VERSION=0.2.0
STAGE="$HERE/stage"; TARBALL="$HERE/gl9-amd64.tar.gz"

# installed-name : built-a.out-basename
demos="gl9-cube:cube_demo gl9-egl:egl_demo"

rm -rf "$STAGE"
mkdir -p "$STAGE/amd64/bin" "$STAGE/rc/bin" "$STAGE/sys/lib/gl9" \
         "$STAGE/sys/lib/pac9/changelog"

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
# `pac9 changelog gl9` reads this — shipped, so it answers "why upgrade?" offline
cp "$HERE/CHANGELOG" "$STAGE/sys/lib/pac9/changelog/gl9"

# ustar format so 9front's tar reads it (no pax/GNU extensions).
( cd "$STAGE" && tar --format ustar -czf "$TARBALL" amd64 rc sys )
echo "-> $TARBALL  ($(du -h "$TARBALL" | cut -f1))"
echo
echo "publish (tag MUST embed the version — the URL has to be immutable):"
echo "  gh release create gl9-v$VERSION '$TARBALL' \\"
echo "     -t 'gl9 $VERSION — OpenGL for 9front (llvmpipe: ~15x faster)' \\"
echo "     -F '$HERE/CHANGELOG'"
echo "then set the registry's gl9 row to version $VERSION with the matching URL."
