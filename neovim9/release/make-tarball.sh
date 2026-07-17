#!/bin/sh
# Build the pac9 tarball for neovim9 (alacritty9/rust9 precedent).
# Layout: /amd64/bin/nvim (cc9 a.out, static: LuaJIT-interp + libuv + parsers),
#         /usr/local/share/nvim/runtime/... (nvim's compiled-in default, no env
#         needed) + empty runtime/parser/<lang>.so markers (discovery is lua;
#         loading is the static table in treesitter.c).
set -e
cd "$(dirname "$0")"

AOUT=../_out/nvim.aout
RT=../_out/nvim-runtime.tgz
[ -f "$AOUT" ] || { echo "missing $AOUT (run port/build-nvim.py)"; exit 1; }
[ -f "$RT" ] || { echo "missing $RT"; exit 1; }

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/amd64/bin" "$stage/usr/local/share/nvim" "$stage/sys/lib/pac9/changelog"
cp "$AOUT" "$stage/amd64/bin/nvim"
chmod +x "$stage/amd64/bin/nvim"
cp CHANGELOG "$stage/sys/lib/pac9/changelog/neovim9"
tar -xzf "$RT" -C "$stage/usr/local/share/nvim"
mkdir -p "$stage/usr/local/share/nvim/runtime/parser"
for p in c lua vim vimdoc query markdown markdown_inline; do
  : > "$stage/usr/local/share/nvim/runtime/parser/$p.so"
done

# ustar so 9front tar reads it; no macOS xattr turds.
COPYFILE_DISABLE=1 tar --format ustar -C "$stage" -czf neovim9-amd64.tar.gz amd64 usr sys
ls -l neovim9-amd64.tar.gz
