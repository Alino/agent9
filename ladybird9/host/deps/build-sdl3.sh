#!/bin/sh
# SDL3 — the REAL library (replaces the old hand-written shim).
#
# Ladybird uses SDL3 for the Gamepad API. The previous port shipped a faithfully
# -empty shim, which meant InternalGamepad's SDL_AttachVirtualJoystick ALWAYS
# failed and every Text/input/GamepadAPI test failed. The virtual joystick is
# pure software (no input hardware, no kernel driver), so the real library works
# on 9front once the platform backends it can't have are switched off.
#
# Build shape: static, libc+pthreads (cc9 has real pthreads), joystick +
# hidapi + virtual-joystick ON, dummy video, everything hardware/desktop OFF.
# NOTE: SDL_VIRTUAL_JOYSTICK is a dep_option on SDL_HIDAPI — with HIDAPI off it
# silently defaults OFF and the virtual driver is not compiled in at all.
# NOTE: cirno's Celeron 3205U has AVX/AVX2 fused off, so those must be OFF or
# the dispatch can execute an unsupported instruction.
. "$(dirname "$0")/common.sh"

SDL_VER=3.2.24
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VER}/SDL3-${SDL_VER}.tar.gz"
SDL_SHA256=81cc0fc17e5bf2c1754eeca9af9c47a76789ac5efdd165b3b91cbbe4b90bfb76

SRC="$VENDOR/sdl3/SDL3-${SDL_VER}"
fetch "$SDL_URL" "$SDL_SHA256" "$SRC"

# Source edits, applied here so a fresh fetch reproduces them. They were once
# hand-made in the extracted tree, which means the next fetch silently dropped
# them and the build broke in three different ways. Each is idempotent.
#
# 1. dynapi: SDL's shared-library ABI shim. It #errors on an unknown platform and
#    deliberately refuses a -D override ("Nope, you have to edit this file"), so
#    the platform case goes in the file, exactly like SDL's own vitasdk/3DS ones.
#    Plan 9 has no dlopen and cc9 links everything statically.
if ! grep -q '__plan9__' "$SRC/src/dynapi/SDL_dynapi.h"; then
	perl -0pi -e 's{(#elif defined\(DYNAPI_NEEDS_DLOPEN\))}{#elif defined(__plan9__)\n#define SDL_DYNAMIC_API 0 // Plan 9 has no dlopen; cc9 links everything statically\n$1}' \
		"$SRC/src/dynapi/SDL_dynapi.h"
	grep -q '__plan9__' "$SRC/src/dynapi/SDL_dynapi.h" || { echo "sdl3: dynapi patch missed" >&2; exit 1; }
fi

# 2. <langinfo.h> was included unconditionally while every nl_langinfo() call
#    below it is already guarded by HAVE_NL_LANGINFO. An SDL bug, not ours.
# NOTE: "#ifdef HAVE_NL_LANGINFO" already occurs in pristine SDL (it guards the
# nl_langinfo() CALLS), so the idempotency marker must be this comment, not it.
if ! grep -q 'already guarded by this' "$SRC/src/time/unix/SDL_systime.c"; then
	perl -0pi -e 's{^#include <langinfo\.h>}{#ifdef HAVE_NL_LANGINFO /* every nl_langinfo() below is already guarded by this */\n#include <langinfo.h>\n#endif}m' \
		"$SRC/src/time/unix/SDL_systime.c"
fi

# 3. Take the strictly-POSIX.1-2008 timezone path (as Solaris does) instead of
#    tm_gmtoff. cc9's struct tm must NOT grow that field: the whole sysroot is
#    prebuilt against the 36-byte layout, so widening it makes gmtime_r write
#    past every struct tm those libraries allocate (it broke OpenSSL's cert
#    expiry check). cc9 supplies tzset() and timezone == 0, which is the true
#    offset on a system whose local time IS UTC.
if ! grep -q '__plan9__' "$SRC/src/time/unix/SDL_systime.c"; then
	perl -0pi -e 's{(\(!defined\(sun\) && !defined\(__sun\))\)}{$1 && !defined(__plan9__))}' \
		"$SRC/src/time/unix/SDL_systime.c"
	grep -q '__plan9__' "$SRC/src/time/unix/SDL_systime.c" || { echo "sdl3: systime patch missed" >&2; exit 1; }
fi

cmake_dep "$SRC" \
	`# NOTE: dynapi (SDL's shared-lib ABI shim) is switched off by a __plan9__` \
	`# case added to src/dynapi/SDL_dynapi.h — SDL refuses a -D override on` \
	`# purpose ("you have to edit this file"), and Plan 9 genuinely has no` \
	`# dlopen, so it takes the same shape as SDL's vitasdk/3DS cases.` \
	-DSDL_STATIC=ON -DSDL_SHARED=OFF -DSDL_TEST_LIBRARY=OFF \
	-DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_TESTS=OFF \
	-DSDL_INSTALL=ON \
	-DSDL_LIBC=ON -DSDL_PTHREADS=ON -DSDL_PTHREADS_SEM=ON \
	\
	`# the whole point: gamepad support` \
	-DSDL_HIDAPI=ON -DSDL_VIRTUAL_JOYSTICK=ON \
	-DSDL_HIDAPI_JOYSTICK=OFF -DSDL_HIDAPI_LIBUSB=OFF \
	\
	`# video: keep the subsystem (SDL_Init paths reference it) but dummy only.` \
	`# SDL_UNIX_CONSOLE_BUILD acknowledges "no X11/Wayland on purpose" — 9front` \
	`# has neither, and Ladybird never asks SDL for a window (gamepad only).` \
	-DSDL_VIDEO=ON -DSDL_DUMMYVIDEO=ON -DSDL_OFFSCREEN=OFF \
	-DSDL_UNIX_CONSOLE_BUILD=ON \
	-DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_VIVANTE=OFF \
	-DSDL_RPI=OFF -DSDL_ROCKCHIP=OFF -DSDL_COCOA=OFF -DSDL_METAL=OFF \
	-DSDL_DIRECTX=OFF -DSDL_XINPUT=OFF \
	-DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF -DSDL_OPENVR=OFF \
	-DSDL_RENDER=OFF -DSDL_GPU=OFF \
	\
	`# no audio/camera/ipc-desktop integration on 9front` \
	-DSDL_AUDIO=OFF -DSDL_CAMERA=OFF \
	-DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF \
	-DSDL_SNDIO=OFF -DSDL_OSS=OFF -DSDL_DISKAUDIO=OFF -DSDL_DUMMYAUDIO=OFF \
	-DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF -DSDL_LIBURING=OFF \
	-DSDL_SYSTEM_ICONV=OFF -DSDL_LIBICONV=OFF \
	\
	`# cirno: no AVX (fused off). SSE2/4 are fine on this part.` \
	-DSDL_AVX=OFF -DSDL_AVX2=OFF -DSDL_AVX512F=OFF \
	-DSDL_ALTIVEC=OFF -DSDL_ARMNEON=OFF -DSDL_LSX=OFF -DSDL_LASX=OFF \
	-DSDL_RPATH=OFF

echo "SDL3 (real) installed into $PREFIX"
