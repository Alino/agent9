#!/bin/sh
# Build the pac9 tarball for dosbox9 (alacritty9/rust9 precedent).
# Layout: /amd64/bin/gl9win2            (prebuilt kencc a.out from a 9front box)
#         /usr/glenda/dosbox9/dosbox    (the cc9 cross-compiled a.out)
#         /usr/glenda/dosbox9/dosbox.conf
#         /rc/bin/dosbox9               (launcher)
#         /sys/lib/pac9/changelog/dosbox9
#
# Games are NOT bundled — they aren't ours to redistribute. host/fetch-games.py
# pulls shareware/freeware from archive.org.
set -e
cd "$(dirname "$0")"

BIN=../_out/dosbox.aout
[ -f "$BIN" ] || { echo "build dosbox first: host/build-sdl.sh && host/build-dosbox.sh"; exit 1; }
[ -f prebuilt/gl9win2 ] || { echo "missing prebuilt/gl9win2 (6c/6l alacritty9/win/gl9win2.c on a 9front box, then fetch it)"; exit 1; }

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/amd64/bin" "$stage/usr/glenda/dosbox9" "$stage/rc/bin" \
         "$stage/sys/lib/pac9/changelog"
cp prebuilt/gl9win2 "$stage/amd64/bin/gl9win2"
cp "$BIN"           "$stage/usr/glenda/dosbox9/dosbox"
cp dosbox.conf      "$stage/usr/glenda/dosbox9/dosbox.conf"
cp dosbox9          "$stage/rc/bin/dosbox9"
# `pac9 changelog dosbox9` reads this — shipped, so it answers "why upgrade?"
# offline.
cp CHANGELOG        "$stage/sys/lib/pac9/changelog/dosbox9"
chmod +x "$stage/amd64/bin/gl9win2" "$stage/usr/glenda/dosbox9/dosbox" "$stage/rc/bin/dosbox9"

# ustar so 9front's tar reads it; COPYFILE_DISABLE so macOS doesn't add
# AppleDouble "._*" turds (they land in the install as junk files).
COPYFILE_DISABLE=1 tar --format ustar -C "$stage" -czf dosbox9-amd64.tar.gz amd64 usr rc sys
ls -l dosbox9-amd64.tar.gz
