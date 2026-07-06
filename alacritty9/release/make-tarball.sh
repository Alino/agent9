#!/bin/sh
# Build the pac9 tarball for alacritty9 (rust9/gl9 precedent).
# Layout: /amd64/bin/gl9win2 (prebuilt kencc a.out, fetched from a build box),
#         /usr/glenda/alacritty9/alacritty (the cross-compiled binary),
#         /rc/bin/alacritty9 (launcher).
set -e
cd "$(dirname "$0")"

BIN=../vendor/alacritty/target/x86_64-unknown-plan9/release/alacritty
[ -f "$BIN" ] || { echo "build alacritty first (cargo +nightly build --release -p alacritty --no-default-features in vendor/alacritty)"; exit 1; }
[ -f prebuilt/gl9win2 ] || { echo "missing prebuilt/gl9win2 (mk in win/ on a 9front box, fetch 6.out)"; exit 1; }

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/amd64/bin" "$stage/usr/glenda/alacritty9" "$stage/rc/bin"
cp prebuilt/gl9win2 "$stage/amd64/bin/gl9win2"
cp "$BIN" "$stage/usr/glenda/alacritty9/alacritty"
cp alacritty9 "$stage/rc/bin/alacritty9"
chmod +x "$stage/amd64/bin/gl9win2" "$stage/usr/glenda/alacritty9/alacritty" "$stage/rc/bin/alacritty9"

# ustar so 9front tar reads it; no macOS xattr turds.
COPYFILE_DISABLE=1 tar --format ustar -C "$stage" -czf alacritty9-amd64.tar.gz amd64 usr rc
ls -l alacritty9-amd64.tar.gz
