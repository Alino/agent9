#!/bin/bash
# build-icu.sh — cross-build ICU 78.3 static libs for 9front via cc9.
#
# Ladybird's CMake demands find_package(ICU 78.3 COMPONENTS uc i18n data);
# this installs a standard-layout ICU into the ladybird9 sysroot:
#   _out/deps/include/unicode/*.h
#   _out/deps/lib/{libicuuc.a,libicui18n.a,libicudata.a}   (target x86_64 ELF)
#   _out/deps/share/icu/78.3/icudt78l.dat                  (33 MB, archive mode)
#
# ICU cross-compiles in two passes: a NATIVE host build first (its tools —
# genrb/icupkg/pkgdata — run at build time), then the cross pass points at it
# with --with-cross-build. Data packaging is `archive` (icudt78l.dat as a
# file, not a 30 MB static lib); libicudata.a is the tiny stub. On-box the
# .dat's final home is /lib/icu/icudt78l.dat (or set ICU_DATA /
# u_setDataDirectory at runtime).
#
# Forced platform defines for the cross pass (cc9 = clang→Plan9, linux-ish
# POSIX surface, configure --host=x86_64-unknown-linux-gnu picks mh-linux):
#   -DU_HAVE_MMAP=0                 cc9 exports mmap but it fails at runtime
#                                   on Plan 9 (no mmap syscall) → configure's
#                                   link-test says "yes"; force MAP_STDIO so
#                                   udata loads the .dat via malloc+fread.
#   -DU_HAVE_NL_LANGINFO_CODESET=0  cc9 has no <langinfo.h>.
#   --disable-dyload                no dlopen plugins on Plan 9 (configure
#                                   adds -DU_ENABLE_DYLOAD=0; it also adds
#                                   -DU_HAVE_POPEN=0 for cross builds).
#
# Host-build ICU bug worked around: with a prebuilt native-endian data
# archive, data/Makefile's $(PKGDATA_LIST) rule shell-redirects into
# out/tmp/ without ever creating it (and races under -j) → pre-create
# data/out/tmp after configure.
#
# Idempotent: every phase is guarded by its artifact; re-running is a no-op.
set -euo pipefail

ICU_VER=78.3
ICU_URL="https://github.com/unicode-org/icu/releases/download/release-${ICU_VER}/icu4c-${ICU_VER}-sources.tgz"
ICU_SHA256=3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"      # ladybird9/host/deps
LB9="$(cd "$HERE/../.." && pwd)"                          # ladybird9/
AGENT9="$(cd "$LB9/.." && pwd)"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
LLD="${CC9_LLD:-$(brew --prefix lld)/bin/ld.lld}"
CC9CC="$AGENT9/servo9/host/cc9-cc"
CC9CXX="$AGENT9/servo9/host/cc9-c++"

VENDOR="$LB9/vendor/icu"
SRC="$VENDOR/icu/source"
BHOST="$VENDOR/build-host"
BCROSS="$VENDOR/build-plan9"
DEPS="$LB9/_out/deps"
NPROC="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

mkdir -p "$VENDOR" "$DEPS"

# --- fetch (pinned) ---------------------------------------------------------
TGZ="$VENDOR/icu4c-${ICU_VER}-sources.tgz"
if [ ! -f "$TGZ" ] || ! echo "$ICU_SHA256  $TGZ" | shasum -a 256 -c - >/dev/null 2>&1; then
	echo "== fetching ICU $ICU_VER"
	curl -fsSL -o "$TGZ" "$ICU_URL"
	echo "$ICU_SHA256  $TGZ" | shasum -a 256 -c -
fi
if [ ! -d "$SRC" ]; then
	echo "== extracting"
	tar xzf "$TGZ" -C "$VENDOR"        # top-level dir is icu/
fi

# --- pass 1: native host build (tools for the cross pass) -------------------
if [ ! -x "$BHOST/bin/pkgdata" ] || [ ! -f "$BHOST/config/icucross.mk" ]; then
	echo "== host build (native, for genrb/icupkg/pkgdata)"
	mkdir -p "$BHOST" && cd "$BHOST"
	"$SRC/runConfigureICU" MacOSX \
		--disable-samples --disable-tests --disable-extras \
		--with-data-packaging=archive > configure-host.log 2>&1
	mkdir -p data/out/tmp              # ICU bug: rule redirects here w/o mkdir
	make -j"$NPROC" > make-host.log 2>&1 || make >> make-host.log 2>&1
fi

# --- pass 2: cross build with cc9 -------------------------------------------
if [ ! -f "$BCROSS/lib/libicuuc.a" ]; then
	echo "== cross build (cc9 -> Plan 9)"
	mkdir -p "$BCROSS" && cd "$BCROSS"
	CC="$CC9CC" CXX="$CC9CXX" \
	AR="$LLVM/llvm-ar" RANLIB="$LLVM/llvm-ranlib" \
	CFLAGS="-O2" CXXFLAGS="-O2" \
	CPPFLAGS="-DU_HAVE_MMAP=0 -DU_HAVE_NL_LANGINFO_CODESET=0" \
	"$SRC/configure" \
		--host=x86_64-unknown-linux-gnu \
		--with-cross-build="$BHOST" \
		--enable-static --disable-shared \
		--disable-dyload --disable-icuio --disable-layoutex \
		--disable-tools --disable-tests --disable-samples --disable-extras \
		--with-data-packaging=archive \
		--prefix="$DEPS" > configure-plan9.log 2>&1
	make -j"$NPROC" > make-plan9.log 2>&1
fi

# --- install into the sysroot ------------------------------------------------
if [ ! -f "$DEPS/lib/libicuuc.a" ] || [ "$BCROSS/lib/libicuuc.a" -nt "$DEPS/lib/libicuuc.a" ]; then
	echo "== install -> $DEPS"
	cd "$BCROSS" && make install > install-plan9.log 2>&1
fi

# --- verify + smoke link ------------------------------------------------------
"$LLVM/llvm-objdump" -f "$BCROSS/common/putil.ao" | grep -q elf64-x86-64
ls -la "$DEPS"/lib/libicu{uc,i18n,data}.a "$DEPS/share/icu/$ICU_VER/icudt78l.dat"

echo "== smoke link (ucnv_open + u_strToUpper + ucol_open)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
"$CC9CXX" -O2 -I "$DEPS/include" -c "$LB9/test/depgates/icu_smoke.cpp" -o "$TMP/icu_smoke.o"
"$LLD" -o "$TMP/icu_smoke.elf" "$TMP/icu_smoke.o" \
	--start-group "$DEPS/lib/libicui18n.a" "$DEPS/lib/libicuuc.a" "$DEPS/lib/libicudata.a" \
	"$AGENT9/cc9/lib/libcc9cxx.a" "$AGENT9/cc9/lib/libcc9m.a" --end-group \
	-T "$AGENT9/cc9/test/plan9.ld" -static -nostdlib
echo "ICU $ICU_VER for 9front: OK"
