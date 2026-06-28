#!/bin/sh
# Reproducibly fetch the exact Zig we port (source) + a host toolchain to
# bootstrap it. Pinned to 0.14.1 with verified shasums.
#
# WHY 0.14.1 (not 0.16.0): Zig's Plan 9 a.out linker backend (src/link/Plan9.zig)
# was REMOVED in 0.15.1 during the "new linker" rework. 0.16/0.15.x return
# error.UnsupportedObjectFormat for the plan9 object format. 0.14.1 is the newest
# release that ships the Plan9 backend AND the self-hosted x86_64 backend (the
# only backend that can emit Plan 9 objects — LLVM cannot). See port/plan9/NOTES.md.
#
#   vendor/zig/        Zig 0.14.1 source (lib/std + src/ compiler + build.zig)
#                      -> what we patch (port/plan9/patches) and cross-build for
#                         x86_64-plan9 (G2).
#   vendor/zig-host/   prebuilt aarch64-macos zig 0.14.1 -> drives `zig build`.
set -e

VERSION=0.14.1
HERE=$(cd "$(dirname "$0")" && pwd)            # zig9/
VENDOR="$HERE/vendor"
mkdir -p "$VENDOR"

SRC_URL="https://ziglang.org/download/$VERSION/zig-$VERSION.tar.xz"
SRC_SHA=237f8abcc8c3fd68c70c66cdbf63dce4fb5ad4a2e6225ac925e3d5b4c388f203
HOST_URL="https://ziglang.org/download/$VERSION/zig-aarch64-macos-$VERSION.tar.xz"
HOST_SHA=39f3dc5e79c22088ce878edc821dedb4ca5a1cd9f5ef915e9b3cc3053e8faefa

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

fetch "$SRC_URL"  "$SRC_SHA"  "$VENDOR/zig"
fetch "$HOST_URL" "$HOST_SHA" "$VENDOR/zig-host"

echo "Zig $VERSION vendored. host binary: $VENDOR/zig-host/zig"
