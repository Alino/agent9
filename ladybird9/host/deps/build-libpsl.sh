#!/bin/sh
# build-libpsl.sh — cross-build libpsl 0.21.5 static for 9front via cc9.
#
# Ladybird REQUIREs `pkg_check_modules(LIBPSL ... libpsl)` and LibURL calls
# psl_builtin()/psl_is_public_suffix2() for cookie-domain security checks, so
# this must be the REAL library with the REAL built-in public suffix list —
# never a stub. Installs into the ladybird9 sysroot:
#   _out/deps/include/libpsl.h
#   _out/deps/lib/libpsl.a
#   _out/deps/lib/pkgconfig/libpsl.pc
#
# Build shape:
#   --enable-builtin --disable-runtime  builtin DAFSA compiled from the
#       release tarball's list/public_suffix_list.dat (psl-make-dafsa is a
#       python script, runs on host python3); no libidn2/libicu/libunistring
#       at runtime. Consequence (documented libpsl behavior, fine for
#       Ladybird which feeds ASCII/punycode hostnames): non-ASCII input to
#       the lookup functions is not lowercased/IDNA-mapped without a runtime
#       IDNA library.
#   make -C src only: tools/ (psl CLI) and tests/ are host-irrelevant and
#       tools/psl.c wants nl_langinfo, which cc9 doesn't have.
#
# Configure-check truthfulness (the try_compile false-positive family): the
# cc9-cc wrapper really links conftests against cc9 libc, and the verify step
# below cross-checks every AC_CHECK_FUNCS result in config.h against
# llvm-nm of libcc9cxx.a — it fails loudly if configure ever claims a
# function cc9 doesn't define (pre-seed ac_cv_func_<fn>=no here if that day
# comes). As of 0.21.5: strndup/clock_gettime/fmemopen yes, nl_langinfo no —
# all match cc9 reality.
. "$(dirname "$0")/common.sh"

PSL_VER=0.21.5
PSL_URL="https://github.com/rockdaboot/libpsl/releases/download/${PSL_VER}/libpsl-${PSL_VER}.tar.gz"
PSL_SHA256=1dcc9ceae8b128f3c0b3f654decd0e1e891afc6ff81098f227ef260449dae208

SRC="$VENDOR/libpsl"
BUILD="$SRC/build-plan9"

fetch "$PSL_URL" "$PSL_SHA256" "$SRC"

# --- configure (cross, static, builtin PSL, no runtime IDNA) -----------------
if [ ! -f "$BUILD/config.h" ]; then
	mkdir -p "$BUILD"
	cd "$BUILD"
	CC="$CC9CC" AR="$AR" RANLIB="$LLVM/llvm-ranlib" NM="$LLVM/llvm-nm" \
	CFLAGS="-O2" PYTHON=python3 \
	"$SRC/configure" \
		--host=x86_64-unknown-linux-gnu \
		--enable-static --disable-shared \
		--enable-builtin --disable-runtime \
		--disable-rpath --disable-gtk-doc --disable-man \
		--prefix="$PREFIX" > configure.log 2>&1
fi

# verify configure's function checks against what cc9 libc actually defines
for fn in strndup clock_gettime fmemopen nl_langinfo; do
	want=n; "$LLVM/llvm-nm" --defined-only "$AGENT9/cc9/lib/libcc9cxx.a" 2>/dev/null \
		| grep -qw "T $fn" && want=y
	got=n; grep -q "define HAVE_$(echo "$fn" | tr a-z A-Z) 1" "$BUILD/config.h" && got=y
	if [ "$want" != "$got" ]; then
		echo "configure lied about $fn (config.h=$got, cc9 libc=$want):" >&2
		echo "  re-run with ac_cv_func_$fn=$([ "$want" = y ] && echo yes || echo no) pre-seeded" >&2
		exit 1
	fi
done

# --- build + install the library only (skip tools/ and tests/) ---------------
if [ ! -f "$BUILD/src/.libs/libpsl.a" ]; then
	make -C "$BUILD/src" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" > "$BUILD/make.log" 2>&1
fi
make -C "$BUILD/src" install > "$BUILD/install.log" 2>&1
make -C "$BUILD/include" install >> "$BUILD/install.log" 2>&1
make -C "$BUILD" install-pkgconfigDATA >> "$BUILD/install.log" 2>&1
rm -f "$PREFIX/lib/libpsl.la"      # libtool metadata, useless in the sysroot

# --- gates --------------------------------------------------------------------
"$LLVM/llvm-objdump" -f "$PREFIX/lib/libpsl.a" | grep -q elf64-x86-64
PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" pkg-config --exists --print-errors libpsl
echo "pkg-config: $(PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" pkg-config --cflags --libs libpsl)"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
"$CC9CC" -O2 -I"$PREFIX/include" -c "$LB9/test/depgates/libpsl_smoke.c" -o "$TMP/libpsl_smoke.o"
LLD="${CC9_LLD:-$(brew --prefix lld)/bin/ld.lld}"
"$LLD" -o "$TMP/libpsl_smoke.elf" \
	--start-group "$TMP/libpsl_smoke.o" "$PREFIX/lib/libpsl.a" \
	"$AGENT9/cc9/lib/libcc9cxx.a" "$AGENT9/cc9/lib/libcc9m.a" --end-group \
	-T "$AGENT9/cc9/test/plan9.ld" -static -nostdlib
"$LLVM/llvm-objdump" -f "$TMP/libpsl_smoke.elf" | grep -q elf64-x86-64

echo "libpsl $PSL_VER (builtin PSL, no runtime IDNA) installed into $PREFIX"
