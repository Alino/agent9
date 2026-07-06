#!/bin/bash
# build-libuv.sh — cross-build libuv 1.52.1 for 9front via cc9, over the
# cc9 poll()/readiness layer. Recipe = libuv's cygwin platform (posix-poll)
# with port/uv-plan9.c as the platform file.
# Output: $OUT/libuv.a + uv headers.
set -euo pipefail
PORT="$(cd "$(dirname "$0")" && pwd)"
NEOVIM9="$(dirname "$PORT")"
OUT="${1:-$NEOVIM9/_out/libuv}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

curl -sL "https://github.com/libuv/libuv/archive/v1.52.1.tar.gz" | tar xz -C "$WORK"
cd "$WORK/libuv-1.52.1"
for p in "$PORT/patches/libuv-"*.patch; do [ -e "$p" ] && patch -p1 < "$p"; done
cp "$PORT/uv-plan9.c" src/unix/plan9.c

COMMON="fs-poll idna inet random strscpy strtok thread-common threadpool timer uv-common uv-data-getter-setters version"
UNIX="async core dl fs getaddrinfo getnameinfo loop-watcher loop pipe poll process
      random-devurandom signal stream tcp thread tty udp
      no-fsevents no-proctitle posix-hrtime posix-poll plan9"

mkdir -p "$WORK/obj"
for f in $COMMON; do
  "$PORT/n9cc" -O2 -Iinclude -Isrc -c "src/$f.c" -o "$WORK/obj/$f.o"
done
for f in $UNIX; do
  "$PORT/n9cc" -O2 -Iinclude -Isrc -Isrc/unix -c "src/unix/$f.c" -o "$WORK/obj/unix_$f.o"
done
/opt/homebrew/opt/llvm/bin/llvm-ar rcs "$WORK/obj/libuv.a" "$WORK/obj"/*.o

mkdir -p "$OUT"
cp "$WORK/obj/libuv.a" "$OUT/"
cp -r include/uv.h include/uv "$OUT/"
echo "built: $OUT/libuv.a"
