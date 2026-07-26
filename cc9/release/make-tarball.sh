#!/bin/sh
# make-tarball.sh — assemble cc9-amd64.tar.gz for pac9's `tarball` install.
#
# Layout at / :
#   /amd64/bin/{cc,clang,ld.lld,elf2aout}    the on-box toolchain
#   /amd64/lib/cc9/{libcc9cxx.a,libcc9m.a}   runtime + math archives
#   /amd64/lib/cc9/{plan9.ld,cc1.template}   link script + cc1 argument template
#   /amd64/lib/cc9/sysinc/{cc9,cxxv1}        C headers + libc++ headers
#   /amd64/lib/cc9/res_rd                    clang's own resource dir
#   /sys/lib/pac9/changelog/cc9
#
# What this script can and cannot rebuild:
#   REBUILT here (host cross-build, from this checkout):
#     libcc9cxx.a  — host/build-runtime.sh; this is the archive every cc9 program
#                    links, so it is what carries runtime fixes to on-box builds
#     libcc9m.a    — host/build-libm.sh (cc9/lib/libcc9m.a)
#     plan9.ld, sysinc/cc9 — plain files copied out of the tree
#   KEPT from the previous release (built natively ON 9front, months of build
#   time, nothing in this repo reproduces them on a Mac):
#     clang, ld.lld, elf2aout, cc, cc1.template, sysinc/cxxv1, res_rd
# So a release cut by this script refreshes the runtime and headers against a
# frozen driver. If the driver or cc1 template ever needs to change, rebuild it
# on the box (native/build-clang.sh, native/cc.rc) and drop it into $CACHE.
#
# Verify a candidate before releasing — extract it on the box and compile
# something real with the on-box cc:
#   cc cc9/test/pollfork_gate.c -o pfg && ./pfg      -> pollfork_gate 6/6 PASS
set -e
HERE=$(cd "$(dirname "$0")" && pwd); CC9=$(dirname "$HERE")
CACHE="$HERE/.cache/base"
PREV=${CC9_PREV_TARBALL:-https://github.com/Alino/agent9/releases/download/cc9-v0.1.0/cc9-amd64.tar.gz}

RT="${CC9_RUNTIME:-$CC9/lib/libcc9cxx.a}"
[ -f "$RT" ] || { echo "missing $RT — run host/build-runtime.sh first"; exit 1; }
[ -f "$CC9/lib/libcc9m.a" ] || { echo "missing libcc9m.a — run host/build-libm.sh first"; exit 1; }

if [ ! -d "$CACHE/amd64/bin" ]; then
  echo "== fetching the native on-box tools from the previous release"
  mkdir -p "$CACHE"
  curl -sL "$PREV" -o "$HERE/.cache/prev.tar.gz"
  tar xzf "$HERE/.cache/prev.tar.gz" -C "$CACHE"
  rm -f "$HERE/.cache/prev.tar.gz"
fi

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp -R "$CACHE/amd64" "$stage/amd64"
mkdir -p "$stage/sys/lib/pac9/changelog"

# the pieces this checkout owns
cp "$RT" "$stage/amd64/lib/cc9/libcc9cxx.a"
cp "$CC9/lib/libcc9m.a" "$stage/amd64/lib/cc9/libcc9m.a"
cp "$CC9/test/plan9.ld" "$stage/amd64/lib/cc9/plan9.ld"
rm -rf "$stage/amd64/lib/cc9/sysinc/cc9"
mkdir -p "$stage/amd64/lib/cc9/sysinc/cc9"
(cd "$CC9/runtime/include" && tar cf - .) | tar xf - -C "$stage/amd64/lib/cc9/sysinc/cc9"
cp "$HERE/CHANGELOG" "$stage/sys/lib/pac9/changelog/cc9"
chmod +x "$stage/amd64/bin"/*

# ustar so 9front's tar reads it; no macOS xattr turds.
COPYFILE_DISABLE=1 tar --format ustar -C "$stage" -czf "$HERE/cc9-amd64.tar.gz" amd64 sys
ls -l "$HERE/cc9-amd64.tar.gz"
