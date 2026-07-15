#!/bin/bash
# deploy9.sh — stage the built Ladybird into a Plan 9 install prefix on the box
# and (optionally) run a headless screenshot. Layout-discovering: it mirrors
# whatever bin/ + libexec/ + share/Lagom the build produced, so it survives
# changes to the resource-files / helper layout.
#
#   deploy9.sh [host] [port] [remote_prefix] [url]
#
# Runtime layout Ladybird expects (LibWebView/Utilities.cpp):
#   <prefix>/bin/ladybird           main + current_executable_path anchor
#   <prefix>/libexec/<Helper>       WebContent/Compositor/RequestServer/...
#   <prefix>/share/Lagom/...        resources (themes, fonts, about pages)
# ICU data is external: ICU_DATA=<prefix>/share/icu (LibUnicode reads it).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LB9="$(cd "$HERE/.." && pwd)"
AGENT9="$(cd "$LB9/.." && pwd)"
BUILD="$LB9/_out/build"
DEPS="$LB9/_out/deps"
CC9="$AGENT9/cc9"

HOST="${1:-192.168.88.159}"
PORT="${2:-17010}"
PREFIX="${3:-/usr/glenda/lb9/ladybird}"
URL="${4:-}"

ship() { python3 "$AGENT9/servo9/host/ship.py" "$1" "$HOST" "$PORT" "$2" >/dev/null; }
rc() { python3 "$HERE/rc9.py" "$HOST" "$PORT" "$1"; }

# elf2aout a SysV-ELF executable to a Plan 9 a.out next to it.
aout() {
    local elf="$1"
    python3 "$CC9/host/elf2aout.py" "$elf" "$elf.aout" >/dev/null 2>&1 || return 1
    echo "$elf.aout"
}

echo "== staging prefix $PREFIX on $HOST:$PORT =="
rc "mkdir -p $PREFIX/bin $PREFIX/libexec $PREFIX/share $PREFIX/share/icu"

# Main binary.
[ -f "$BUILD/bin/ladybird" ] || { echo "no $BUILD/bin/ladybird — build M4 first"; exit 1; }
ship "$(aout "$BUILD/bin/ladybird")" "$PREFIX/bin/ladybird"

# Helper processes (elf2aout each; they live in libexec/ per find_prefix()).
for helper in Compositor ImageDecoder RequestServer WebContent WebWorker; do
    for cand in "$BUILD/bin/$helper" "$BUILD/libexec/$helper" "$BUILD/lib/ladybird/$helper"; do
        if [ -f "$cand" ]; then
            ship "$(aout "$cand")" "$PREFIX/libexec/$helper"
            echo "  helper $helper"
            break
        fi
    done
done

# Resources: mirror the built share/Lagom tree (tar over the wire).
LAGOM=""
for cand in "$BUILD/share/Lagom" "$BUILD/Lagom/share/Lagom"; do
    [ -d "$cand" ] && LAGOM="$cand" && break
done
if [ -n "$LAGOM" ]; then
    tmptar="$LB9/_out/lagom-res.tar.gz"
    tar -C "$LAGOM/.." -czf "$tmptar" "$(basename "$LAGOM")"
    ship "$tmptar" "$PREFIX/share/Lagom.tar.gz"
    rc "cd $PREFIX/share && gunzip < Lagom.tar.gz | tar xf - && rm Lagom.tar.gz"
    echo "  resources <- $LAGOM"
else
    echo "  WARN: no built share/Lagom found; resources missing"
fi

# ICU data archive.
ICU_DAT="$DEPS/share/icu/78.3/icudt78l.dat"
[ -f "$ICU_DAT" ] && ship "$ICU_DAT" "$PREFIX/share/icu/icudt78l.dat" && echo "  icu data"

echo "== staged =="

if [ -n "$URL" ]; then
    echo "== headless screenshot of $URL =="
    rc "rm -f $PREFIX/shot.png $PREFIX/hl.out
        { ICU_DATA=$PREFIX/share/icu $PREFIX/bin/ladybird --headless --screenshot-path=$PREFIX/shot.png $URL > $PREFIX/hl.out >[2=1] & }
        echo LAUNCHED"
    echo "  (poll $PREFIX/hl.out and $PREFIX/shot.png; pull with ship-back)"
fi
