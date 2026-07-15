#!/bin/sh
# Fetch the pinned upstream tree into vendor/. Re-running overwrites vendor/ —
# our patches live in git history + PORT-NOTES.md, so only run this to re-pin.
set -e
cd "$(dirname "$0")"

DOSBOX_VER=0.74-3
URL="https://sourceforge.net/projects/dosbox/files/dosbox/$DOSBOX_VER/dosbox-$DOSBOX_VER.tar.gz/download"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

curl -sL -o "$tmp/dosbox.tar.gz" "$URL"
tar xzf "$tmp/dosbox.tar.gz" -C "$tmp"

mkdir -p vendor
rm -rf "vendor/dosbox-$DOSBOX_VER"
mv "$tmp/dosbox-$DOSBOX_VER" vendor/

echo "dosbox $DOSBOX_VER" > vendor/PINS
