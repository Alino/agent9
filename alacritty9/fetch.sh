#!/bin/sh
# Fetch the pinned upstream trees into vendor/. Re-running overwrites vendor/ —
# our patches live in git history + PORT-NOTES.md diffs, so only run this to re-pin.
set -e
cd "$(dirname "$0")"

ALACRITTY_TAG=v0.17.0
WINIT_TAG=v0.30.13

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

git clone --depth 1 --branch $ALACRITTY_TAG https://github.com/alacritty/alacritty "$tmp/alacritty"
git clone --depth 1 --branch $WINIT_TAG https://github.com/rust-windowing/winit "$tmp/winit"
rm -rf "$tmp/alacritty/.git" "$tmp/winit/.git"

mkdir -p vendor
rm -rf vendor/alacritty vendor/winit
mv "$tmp/alacritty" vendor/alacritty
mv "$tmp/winit" vendor/winit

echo "alacritty $ALACRITTY_TAG" > vendor/PINS
echo "winit $WINIT_TAG" >> vendor/PINS
