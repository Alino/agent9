#!/bin/bash
# build-deps.sh — cross-compile neovim's remaining C deps for 9front via cc9:
# unibilium, utf8proc, tree-sitter, lpeg, luv(+compat53), and the 6 bundled
# treesitter parsers (as static archives; Plan 9 has no dlopen — G4 patches
# nvim's loader with a static table).
# Sources: the host build's .deps tree (vendor/neovim/.deps/build/src/*).
set -euo pipefail
PORT="$(cd "$(dirname "$0")" && pwd)"
NEOVIM9="$(dirname "$PORT")"
DEPS="$NEOVIM9/vendor/neovim/.deps/build/src"
OUT="${1:-$NEOVIM9/_out/deps}"
AR="/opt/homebrew/opt/llvm/bin/llvm-ar"
CC="$PORT/n9cc"
mkdir -p "$OUT/obj"

build_lib() { # name, then object files already in $OUT/obj
  local name="$1"; shift
  $AR rcs "$OUT/lib$name.a" "$@"
  echo "lib$name.a"
}

# unibilium (terminfo)
objs=()
for f in "$DEPS"/unibilium/*.c; do
  o="$OUT/obj/unibilium_$(basename "$f" .c).o"
  $CC -O2 -I"$DEPS/unibilium" -DTERMINFO_DIRS='"/lib/terminfo"' -c "$f" -o "$o"
  objs+=("$o")
done
build_lib unibilium "${objs[@]}"

# utf8proc
$CC -O2 -I"$DEPS/utf8proc" -DUTF8PROC_STATIC -c "$DEPS/utf8proc/utf8proc.c" -o "$OUT/obj/utf8proc.o"
build_lib utf8proc "$OUT/obj/utf8proc.o"

# tree-sitter (amalgamation)
$CC -O2 -I"$DEPS/treesitter/lib/include" -I"$DEPS/treesitter/lib/src" -DHAVE_ENDIAN_H \
  -c "$DEPS/treesitter/lib/src/lib.c" -o "$OUT/obj/treesitter.o"
build_lib tree-sitter "$OUT/obj/treesitter.o"

# lpeg
objs=()
for f in "$DEPS"/lpeg/*.c; do
  o="$OUT/obj/lpeg_$(basename "$f" .c).o"
  $CC -O2 -I"$NEOVIM9/_out/luajit" -c "$f" -o "$o"
  objs+=("$o")
done
build_lib lpeg "${objs[@]}"

# luv + lua-compat-5.3
$CC -O2 -I"$DEPS/luv/src" -I"$DEPS/lua_compat53/c-api" \
  -I"$NEOVIM9/_out/luajit" -I"$NEOVIM9/_out/libuv" \
  -DLUV_LIBUV_HAS_METRICS_IDLE_TIME=0 \
  -c "$DEPS/luv/src/luv.c" -o "$OUT/obj/luv.o"
$CC -O2 -I"$DEPS/lua_compat53/c-api" -I"$NEOVIM9/_out/luajit" \
  -c "$DEPS/lua_compat53/c-api/compat-5.3.c" -o "$OUT/obj/compat53.o"
build_lib luv "$OUT/obj/luv.o" "$OUT/obj/compat53.o"

# treesitter parsers (static)
for lang in c lua vim vimdoc query markdown; do
  d="$DEPS/treesitter_$lang"
  objs=()
  if [ "$lang" != markdown ]; then
    src="$d/src"
    for f in "$src"/*.c; do
      o="$OUT/obj/tsp_${lang}_$(basename "$f" .c).o"
      $CC -O2 -I"$src" -I"$DEPS/treesitter/lib/include" -c "$f" -o "$o"
      objs+=("$o")
    done
  else
    # markdown ships two grammars in subdirs
    for sub in tree-sitter-markdown tree-sitter-markdown-inline; do
      for f in "$d/$sub/src"/*.c; do
        o="$OUT/obj/tsp_md_${sub##*-}_$(basename "$f" .c).o"
        $CC -O2 -I"$d/$sub/src" -I"$DEPS/treesitter/lib/include" -c "$f" -o "$o"
        objs+=("$o")
      done
    done
  fi
  build_lib "tsparser_$lang" "${objs[@]}"
done
echo "built: $OUT"
