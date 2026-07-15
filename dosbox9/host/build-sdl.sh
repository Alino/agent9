#!/bin/bash
# build-sdl.sh — build libSDL.a (SDL 1.2.15 + our Plan 9 backends) for the
# cc9 -> 9front amd64 target.
#
# We bypass SDL's autoconf entirely: SDL_config.h is hand-written
# (port/plan9/SDL_config.h) and the file list is explicit below. That's less
# work than teaching configure about a target it will never see.
#
# Re-runnable: syncs port/plan9/ into the vendor tree and registers the
# backends in SDL's two bootstrap arrays each time (idempotent).
set -euo pipefail
D9="$(cd "$(dirname "$0")/.." && pwd)"
CC9="$(cd "$D9/.." && pwd)/cc9"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
SDL="$D9/vendor/SDL-1.2.15"
PORT="$D9/port/plan9"
O="${O:-/tmp/dosbox9/sdl}"

rm -rf "$O"; mkdir -p "$O" "$D9/lib"

# --- sync our backends into the vendor tree ---------------------------------
mkdir -p "$SDL/src/video/plan9" "$SDL/src/audio/plan9"
cp "$PORT/SDL_config.h"    "$SDL/include/SDL_config.h"
cp "$PORT/SDL_p9video.c" "$PORT/SDL_p9video.h" "$SDL/src/video/plan9/"
cp "$PORT/SDL_p9audio.c" "$PORT/SDL_p9audio.h" "$SDL/src/audio/plan9/"

# --- register the backends in SDL's bootstrap arrays (idempotent) -----------
# NB: anchor the extern decls on the *unconditional* "current video/audio
# device" comment, NOT on a neighbouring driver's extern — those all sit inside
# `#if SDL_VIDEO_DRIVER_<X>` guards, and an extern inserted there silently
# compiles away when that driver is off (which cost us a confusing build once).
python3 - "$SDL" <<'PY'
import re, sys
sdl = sys.argv[1]

def register(path, anchor, block, marker):
    s = open(path).read()
    # drop any previous (possibly misplaced) insertion, then re-insert cleanly
    s = "\n".join(l for l in s.split("\n") if marker not in l)
    s = s.replace(anchor, block + anchor, 1)
    open(path, "w").write(s)

register(f"{sdl}/src/video/SDL_sysvideo.h",
         "/* This is the current video device */",
         "#if SDL_VIDEO_DRIVER_P9\nextern VideoBootStrap P9_bootstrap;\n#endif\n\n",
         "extern VideoBootStrap P9_bootstrap;")

register(f"{sdl}/src/audio/SDL_sysaudio.h",
         "/* This is the current audio device */",
         "#if SDL_AUDIO_DRIVER_P9\nextern AudioBootStrap P9AUD_bootstrap;\n#endif\n\n",
         "extern AudioBootStrap P9AUD_bootstrap;")

# bootstrap arrays
p = f"{sdl}/src/video/SDL_video.c"
s = open(p).read()
if "P9_bootstrap" not in s:
    s = s.replace("static VideoBootStrap *bootstrap[] = {",
                  "static VideoBootStrap *bootstrap[] = {\n#if SDL_VIDEO_DRIVER_P9\n\t&P9_bootstrap,\n#endif")
    open(p, "w").write(s)

p = f"{sdl}/src/audio/SDL_audio.c"
s = open(p).read()
if "P9AUD_bootstrap" not in s:
    s = s.replace("static AudioBootStrap *bootstrap[] = {",
                  "static AudioBootStrap *bootstrap[] = {\n#if SDL_AUDIO_DRIVER_P9\n\t&P9AUD_bootstrap,\n#endif")
    open(p, "w").write(s)
print("bootstrap registration ok")
PY

# --- compile ---------------------------------------------------------------
# Match cc9's user-code flags (cc9/host/cc9). No -ffreestanding.
FLAGS=(--target=x86_64-unknown-none -nostdlib -DNDEBUG -O2
       -femulated-tls -funwind-tables -fno-pic
       -isystem "$CC9/runtime/include"
       -I"$SDL/include" -I"$SDL/src" -I"$SDL/src/video" -I"$SDL/src/audio"
       -I"$SDL/src/events" -I"$SDL/src/thread" -I"$SDL/src/timer"
       -I"$SDL/src/joystick" -I"$SDL/src/cdrom"
       -Wno-register -Wno-implicit-function-declaration)

SRCS=(
  "$SDL"/src/*.c
  "$SDL"/src/stdlib/*.c
  "$SDL"/src/audio/SDL_audio.c "$SDL"/src/audio/SDL_audiocvt.c
  "$SDL"/src/audio/SDL_audiodev.c "$SDL"/src/audio/SDL_mixer.c
  "$SDL"/src/audio/SDL_wave.c
  "$SDL"/src/audio/plan9/*.c
  "$SDL"/src/cdrom/SDL_cdrom.c "$SDL"/src/cdrom/dummy/*.c
  "$SDL"/src/cpuinfo/*.c
  "$SDL"/src/events/*.c
  "$SDL"/src/file/*.c
  "$SDL"/src/joystick/SDL_joystick.c "$SDL"/src/joystick/dummy/*.c
  "$SDL"/src/loadso/dummy/*.c
  "$SDL"/src/thread/SDL_thread.c "$SDL"/src/thread/pthread/*.c
  "$SDL"/src/timer/SDL_timer.c "$SDL"/src/timer/unix/*.c
  "$SDL"/src/video/*.c
  "$SDL"/src/video/plan9/*.c
)

ok=0; fail=0
for f in "${SRCS[@]}"; do
  [ -f "$f" ] || continue
  o="$O/$(echo "${f#$SDL/}" | tr '/' '_').o"
  if "$LLVM/clang" "${FLAGS[@]}" -c "$f" -o "$o" 2> "$o.log"; then
    ok=$((ok+1)); rm -f "$o.log"
  else
    fail=$((fail+1)); echo "FAIL $f"; head -4 "$o.log" | sed 's/^/    /'
  fi
done
echo "SDL: ok=$ok fail=$fail"
[ "$fail" -eq 0 ] || exit 1

"$LLVM/llvm-ar" rcs "$D9/lib/libSDL.a" "$O"/*.o
echo "wrote $D9/lib/libSDL.a ($(ls -la "$D9/lib/libSDL.a" | awk '{print $5}') bytes)"
