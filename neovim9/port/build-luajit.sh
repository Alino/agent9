#!/bin/bash
# build-luajit.sh — cross-build LuaJIT (interpreter mode) for 9front via cc9.
# Source: the commit pinned by neovim's cmake.deps (see vendor/neovim/cmake.deps/deps.txt).
# Output: $OUT/libluajit.a + $OUT/lj9.aout (standalone CLI, gate runner) + lua headers.
# Host tools (minilua/buildvm) build natively; target objects via port/n9cc.
set -euo pipefail
PORT="$(cd "$(dirname "$0")" && pwd)"
NEOVIM9="$(dirname "$PORT")"
OUT="${1:-$NEOVIM9/_out/luajit}"
LJ_COMMIT=fbb36bb6bfa88716a47c58bcf9ce9f2ef752abac
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

curl -sL "https://github.com/luajit/luajit/archive/$LJ_COMMIT.tar.gz" | tar xz -C "$WORK"
cd "$WORK/luajit-$LJ_COMMIT"
patch -p1 < "$PORT/patches/luajit-lj_prng-plan9.patch"

make -C src -j8 HOST_CC="clang -O2" CC="$PORT/n9cc" TARGET_SYS=Other \
  TARGET_AR="/opt/homebrew/opt/llvm/bin/llvm-ar rcus" TARGET_STRIP=: \
  XCFLAGS="-DLUAJIT_DISABLE_JIT -DLUAJIT_USE_SYSMALLOC -DLUAJIT_NO_UNWIND" \
  BUILDMODE=static libluajit.a

"$PORT/n9cc" -O2 -c src/luajit.c -o src/luajit_main.o -DLUAJIT_DISABLE_JIT
"$PORT/n9link" -o src/lj9.aout src/luajit_main.o src/libluajit.a

mkdir -p "$OUT"
cp src/libluajit.a src/lj9.aout "$OUT/"
cp src/lua.h src/lauxlib.h src/lualib.h src/luaconf.h src/lua.hpp src/luajit.h "$OUT/"
echo "built: $OUT/{libluajit.a,lj9.aout,headers}"
