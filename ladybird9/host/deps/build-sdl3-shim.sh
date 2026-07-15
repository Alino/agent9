#!/bin/sh
# SDL3 — SHIM, not the real library (see port/sdl3-shim/sdl3-shim.c for why:
# Ladybird uses SDL3 only for gamepads; 9front has none, so the shim behaves
# like real SDL3 with the dummy joystick backend). The HEADERS are the real,
# pinned SDL3 3.2.24 release headers so every consumer compiles against the
# genuine API; only the implementation is faithfully-empty.
# Installs include/SDL3/, libSDL3.a, and a CONFIG package so Ladybird's
# find_package(SDL3 CONFIG REQUIRED) + SDL3::SDL3 resolve unchanged.
. "$(dirname "$0")/common.sh"

SDL_VER=3.2.24
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VER}/SDL3-${SDL_VER}.tar.gz"
SDL_SHA256=81cc0fc17e5bf2c1754eeca9af9c47a76789ac5efdd165b3b91cbbe4b90bfb76

SHIM="$LB9/port/sdl3-shim"
SDLHDRS="$VENDOR/sdl3/include/SDL3"

# --- vendor the official release headers (headers only, pinned) -------------
if [ ! -f "$SDLHDRS/SDL_gamepad.h" ]; then
	tmp=$(mktemp -d)
	echo "fetching $SDL_URL"
	curl -fsSL -o "$tmp/sdl3.tgz" "$SDL_URL"
	got=$(shasum -a 256 "$tmp/sdl3.tgz" | awk '{print $1}')
	if [ "$got" != "$SDL_SHA256" ]; then
		echo "shasum mismatch for $SDL_URL (want $SDL_SHA256 got $got)" >&2
		rm -rf "$tmp"; exit 1
	fi
	tar xzf "$tmp/sdl3.tgz" -C "$tmp" "SDL3-${SDL_VER}/include"
	mkdir -p "$VENDOR/sdl3/include"
	rm -rf "$SDLHDRS"
	mv "$tmp/SDL3-${SDL_VER}/include/SDL3" "$SDLHDRS"
	rm -rf "$tmp"
	echo "-> $SDLHDRS"
fi

# --- build the shim against the real headers ---------------------------------
# -Werror guards: the shim must match real SDL3 signatures exactly.
"$CC9CC" -O2 -Werror=incompatible-pointer-types -Werror=implicit-function-declaration \
	-I"$VENDOR/sdl3/include" -c "$SHIM/sdl3-shim.c" -o "$VENDOR/sdl3/sdl3-shim.o"
"$AR" rcs "$PREFIX/lib/libSDL3.a" "$VENDOR/sdl3/sdl3-shim.o"

# --- install headers + CONFIG package ----------------------------------------
mkdir -p "$PREFIX/include/SDL3" "$PREFIX/lib/cmake/SDL3"
cp "$SDLHDRS"/*.h "$PREFIX/include/SDL3/"
cp "$SHIM/SDL3Config.cmake" "$SHIM/SDL3ConfigVersion.cmake" "$PREFIX/lib/cmake/SDL3/"

# --- gates --------------------------------------------------------------------
# 1. every SDL_* symbol the Ladybird consumers reference is defined in the .a
LADYBIRD="$LB9/vendor/ladybird"
refs=$(grep -rhoE 'SDL_[A-Za-z0-9_]+\(' \
		"$LADYBIRD/Services/WebContent/main.cpp" \
		"$LADYBIRD/Libraries/LibWeb/Gamepad/"*.cpp \
		"$LADYBIRD/Libraries/LibWeb/Page/EventHandler.cpp" \
	| tr -d '(' | sort -u)
defined=$("$LLVM/llvm-nm" --defined-only "$PREFIX/lib/libSDL3.a" | awk '$2=="T"{print $3}' | tr '\n' ' ')
missing=0
for s in $refs; do
	case " $defined " in
	*" $s "*) ;;
	*) echo "MISSING from libSDL3.a: $s" >&2; missing=1 ;;
	esac
done
[ "$missing" = 0 ] || exit 1

# 2. smoke: compile + link a real caller the way Ladybird binaries link
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
"$CC9CC" -O2 -I"$PREFIX/include" -c "$LB9/test/depgates/sdl3_smoke.c" -o "$TMP/sdl3_smoke.o"
LLD="${CC9_LLD:-$(brew --prefix lld)/bin/ld.lld}"
"$LLD" -o "$TMP/sdl3_smoke.elf" \
	--start-group "$TMP/sdl3_smoke.o" "$PREFIX/lib/libSDL3.a" \
	"$AGENT9/cc9/lib/libcc9cxx.a" "$AGENT9/cc9/lib/libcc9m.a" --end-group \
	-T "$AGENT9/cc9/test/plan9.ld" -static -nostdlib
"$LLVM/llvm-objdump" -f "$TMP/sdl3_smoke.elf" | grep -q elf64-x86-64

echo "SDL3 $SDL_VER headers + gamepad shim installed into $PREFIX ($(echo "$refs" | wc -l | tr -d ' ') consumer symbols covered)"
