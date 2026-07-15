#!/bin/bash
# build-dosbox.sh — build DOSBox 0.74-3 into a 9front amd64 a.out via cc9.
#
# Prereqs: cc9/host/build-runtime.sh (libcc9cxx.a, libcc9m.a) and
#          dosbox9/host/build-sdl.sh (libSDL.a).
# Output:  dosbox9/_out/dosbox.aout
set -euo pipefail
D9="$(cd "$(dirname "$0")/.." && pwd)"
CC9="$(cd "$D9/.." && pwd)/cc9"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
LLD="${CC9_LLD:-$(brew --prefix lld)/bin/ld.lld}"
LIBCXX="${CC9_LIBCXX:-/tmp/libcxx-thr/include/c++/v1}"
DBX="$D9/vendor/dosbox-0.74-3"
SDL="$D9/vendor/SDL-1.2.15"
O="${O:-/tmp/dosbox9/dbx}"

mkdir -p "$O" "$D9/_out"

# --- config.h (autoconf substitution; keeps config.h.in's typedef tail) -----
python3 "$D9/host/genconfig.py" "$DBX/config.h.in" "$DBX/config.h"

# --- source patches (idempotent) -------------------------------------------
python3 - "$DBX" <<'PY'
import sys
dbx = sys.argv[1]

# 1. GFX_ShowMsg logs with printf -> stdout. Under gl9win2, stdout IS the GL9F
#    frame stream, so a single log line corrupts the framing and the window
#    dies. Send diagnostics to stderr (fd 2 is a passthrough).
p = f"{dbx}/src/gui/sdlmain.cpp"
s = open(p).read()
if 'if(!no_stdout) printf("%s",buf);' in s:
    s = s.replace('if(!no_stdout) printf("%s",buf);',
                  'if(!no_stdout) fprintf(stderr,"%s",buf); /* p9: stdout is the frame stream */')
    open(p, "w").write(s)
    print("patched: GFX_ShowMsg -> stderr")

# NOTE for whoever picks up the GL9D delta bug (see PORT-NOTES.md):
# A tempting theory was RENDER_EndUpdate(abort=true) — it calls
# GFX_EndUpdate(NULL) (reporting nothing) even though the scaler has already
# written lines into the surface, which a damage-tracking presenter would lose
# forever. It was TESTED: presenting the whole surface on that path
# (SDL_UpdateRect(sdl.surface,0,0,0,0) in GFX_EndUpdate's else branch) changed
# nothing — bmenace still showed "xx414". So aborts are either not happening
# here or are not the cause. Don't re-litigate it without new evidence.
PY

# --- compile ---------------------------------------------------------------
FLAGS=(--target=x86_64-unknown-none -nostdlib -DNDEBUG -O2
       -D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE -D_LIBCPP_HAS_CLOCK_GETTIME
       -femulated-tls -funwind-tables -fno-pic
       -std=c++23 -nostdinc++ -isystem "$LIBCXX" -isystem "$CC9/runtime/include"
       -I"$DBX" -I"$DBX/include" -I"$DBX/src/libs/gui_tk" -I"$SDL/include"
       -fexceptions -frtti
       -Wno-register)   # render_templates_sai.h: `register` is a C++17 error

ok=0; fail=0
for f in $(find "$DBX/src" -name "*.cpp" | sort); do
  case "$f" in
    */libs/zmbv/*) continue ;;   # AVI capture codec: needs zlib, C_SSHOT is off
  esac
  o="$O/$(echo "${f#$DBX/src/}" | tr '/' '_').o"
  if "$LLVM/clang++" "${FLAGS[@]}" -c "$f" -o "$o" 2> "$o.log"; then
    ok=$((ok+1)); rm -f "$o.log"
  else
    fail=$((fail+1)); echo "FAIL $f"; grep -m2 "error:" "$o.log" | sed 's/^/    /'
  fi
done
echo "DOSBox: ok=$ok fail=$fail"
[ "$fail" -eq 0 ] || exit 1

# --- link ------------------------------------------------------------------
# --start-group: libSDL/libcc9cxx/libcc9m are mutually dependent.
"$LLD" -o "$O/dosbox.elf" "$O"/*.o \
  --start-group "$D9/lib/libSDL.a" "$CC9/lib/libcc9cxx.a" "$CC9/lib/libcc9m.a" --end-group \
  -T "$CC9/test/plan9.ld" -static -nostdlib

python3 "$CC9/host/elf2aout.py" "$O/dosbox.elf" "$D9/_out/dosbox.aout"
ls -la "$D9/_out/dosbox.aout"
