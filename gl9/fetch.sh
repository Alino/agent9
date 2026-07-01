#!/bin/sh
# Reproducibly fetch the exact Mesa we port. Pinned to 24.0.9 with a verified
# shasum.
#
# WHY 24.0.9 (not latest): softpipe (the pure-C gallium software rasterizer) has
# been GL 3.3 / GLES 3.x complete for years, so any recent Mesa clears our floor
# (Alacritty needs GL 3.3 core / GLES2 + shaders). We pin <= 24.0.x because Rust
# entered Mesa's build for some components in 24.1+, and this repo has a "no Rust"
# rule with no rust9 toolchain. 24.0.x is settled/"boring" and predates newer hard
# deps (libdrm assumptions, more codegen). See port/plan9/README.md.
#
#   vendor/mesa/   Mesa 24.0.9 source -> what host/linux-configure.sh runs meson
#                  over (generating C + compile_commands.json), then cc9 compiles.
set -e

VERSION=24.0.9
HERE=$(cd "$(dirname "$0")" && pwd)            # gl9/
VENDOR="$HERE/vendor"
mkdir -p "$VENDOR"

SRC_URL="https://archive.mesa3d.org/mesa-$VERSION.tar.xz"
SRC_SHA=51aa686ca4060e38711a9e8f60c8f1efaa516baf411946ed7f2c265cd582ca4c

# fetch URL SHA DESTDIR — moves the single extracted top-level dir to DESTDIR
# (don't assume the archive's internal dir name).
fetch() {
	url=$1; sha=$2; dest=$3
	if [ -d "$dest" ]; then
		echo "already present: $dest"
		return 0
	fi
	tmp=$(mktemp -d)
	echo "fetching $url"
	curl -fsSL -o "$tmp/a.tar.xz" "$url"
	got=$(shasum -a 256 "$tmp/a.tar.xz" | awk '{print $1}')
	if [ "$got" != "$sha" ]; then
		echo "shasum mismatch for $url" >&2
		echo "  want $sha" >&2
		echo "  got  $got" >&2
		rm -rf "$tmp"; exit 1
	fi
	mkdir "$tmp/x"
	tar xJf "$tmp/a.tar.xz" -C "$tmp/x"
	inner=$(find "$tmp/x" -maxdepth 1 -mindepth 1 -type d | head -1)
	mv "$inner" "$dest"
	rm -rf "$tmp"
	echo "-> $dest"
}

fetch "$SRC_URL" "$SRC_SHA" "$VENDOR/mesa"

echo "Mesa $VERSION vendored at $VENDOR/mesa"
