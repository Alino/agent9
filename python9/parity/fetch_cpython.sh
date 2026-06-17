#!/bin/sh
# Reproducibly fetch the exact CPython source we port + validate against.
# The same tree is built on the host (reference oracle) and cross-built for
# 9front, so Lib/test is byte-identical on both sides.
set -e

VERSION=3.11.14
HERE=$(cd "$(dirname "$0")/.." && pwd)   # python9/
DEST="$HERE/cpython/src"
URL="https://www.python.org/ftp/python/$VERSION/Python-$VERSION.tgz"

if [ -d "$DEST" ]; then
	echo "already present: $DEST"
	exit 0
fi

tmp=$(mktemp -d)
echo "fetching $URL"
curl -fsSL -o "$tmp/py.tgz" "$URL"
tar xzf "$tmp/py.tgz" -C "$tmp"
mv "$tmp/Python-$VERSION" "$DEST"
rm -rf "$tmp"
echo "CPython $VERSION -> $DEST"
