# dosbox9 — DOSBox 0.74-3 on 9front via cc9

**Status: DOSBox runs on 9front. 12/12 games launch and render** on bare-metal
cirno — Doom, Wolfenstein 3D, Commander Keen 1 & 4, Jill of the Jungle, Crystal
Caves, Raptor, Blake Stone, Duke Nukem II, Bio Menace, Halloween Harry, Hocus
Pocus. Boots to `Z:\>`; keyboard works end-to-end (`dir` lists the drive).
Screenshots in `screenshots/` (`host/shoot-games.sh` regenerates them).

Not yet: **sound is untested** (the VM has no `/dev/audio`; cirno has one and
the write path runs, but nobody has listened). The GL9D delta path is buggy so
we present full frames — correct but slow (see below). Nothing has been played
for more than a title screen.

```
host: build-sdl.sh -> lib/libSDL.a       (SDL 1.2.15 + our plan9 backends)
      build-dosbox.sh -> _out/dosbox.aout (118 .cpp + libSDL + libcc9cxx)
      fetch-games.py / prep-games.sh / gen-confs.py -> the C: tree
guest: gl9win2 (native kencc, owns the rio window) hosts dosbox over 3 fds
```

## Architecture: why there's no libdraw in this port

cc9 emits a **System-V-ABI** Plan 9 a.out; libdraw is **kencc** objects. They
cannot link. So dosbox9 does not draw — it speaks the **gl9win2** protocol
(`alacritty9/PROTOCOL.md`) to a native window server that already existed:

| fd | direction | contents |
|----|-----------|----------|
| 0  | win → app | 16-byte big-endian event records |
| 1  | app → win | `GL9F` full frames / `GL9D` damage rects / `GL9T` title |
| 2  | app → win | stderr passthrough |

`gl9win2` was reused **unmodified** from alacritty9. It already does raw
`/dev/kbd` with key-up + modifiers, mouse, resize, and child spawn.

## The port is SDL, not DOSBox

DOSBox itself: **118/118 .cpp compile clean** at `-std=c++23`, no source
changes beyond one patch (below). The whole port is:

- `port/plan9/SDL_config.h` — hand-written (SDL's configure can't see cc9)
- `port/plan9/SDL_p9video.c` — video + events over the gl9win2 protocol
- `port/plan9/SDL_p9audio.c` — `/dev/audio`

We **vendored real SDL 1.2.15** rather than hand-rolling a mini-SDL. Its
generic layer gives us the blitters, palette→RGB conversion, and audio format
conversion — exactly what DOSBox leans on and what a hand-rolled shim gets
subtly wrong. SDL's own `dummy` backends cover cdrom/joystick/loadso for free.

## OPEN BUG: the GL9D delta path loses updates

**Default is full frames (`P9_DELTAS=1` opts into deltas). Deltas are wrong.**

Reproduce: `P9_DELTAS=1`, run `bmenace` or `keen4` (Apogee "Galaxy" engine),
let the screen settle, screenshot. The memory counter reads `xx414` and the
status box `Loaddyg..PresPleaseyWait` — old text left under new. It **persists
on a static screen**, so it is lost state, not a mid-animation capture. The
same run with full frames is clean. `harry` shows a garbled horizontal band;
`wolf3d` speckles.

Ruled out, each by measurement, not argument:

- our frame is byte-correct — `P9_DUMP_FRAME=N` writes it as a PPM; pull it
  back and look (it's perfect)
- gl9win2 drops no `GL9D` — instrumented its out-of-range `else if` branch
  (which silently discards): **zero** hits
- `loadimage` never short-loads — instrumented its return: zero failures
- wire format matches: 4-byte magic + 16-byte x/y/w/h, rows packed at the
  RECT's stride (`bytesperline` = Dx*4), which is what `loadimage` wants
- nothing else writes fd 1 (`GFX_ShowMsg` is patched to stderr; the remaining
  `printf`s are in help/error paths)
- there is no SDL shadow surface (`shadow=0` in `P9_VIDEO_DEBUG`), so
  `this->screen` IS DOSBox's surface
- DOSBox's rects are honest: `GFX_EndUpdate` builds them from `changedLines`
  run-length data (even index = unchanged run, odd = changed run) — the same
  mechanism that makes partial updates work on SDL/X11

Theories TESTED AND DISPROVED (don't re-litigate without new evidence):

- *`loadimage` short-loads and the reused image keeps stale rows.* Instrumented
  its return value: zero failures. It transfers all 1,024,000 bytes every time.
- *`RENDER_EndUpdate(abort=true)` loses writes.* It calls `GFX_EndUpdate(NULL)`
  — reporting nothing — while `render.scale.outWrite` is non-NULL, i.e. the
  scaler already wrote those lines into the surface. Beautiful theory: a
  damage-tracking presenter never gets a second chance at them, whereas
  SDL_Flip/X11 sweeps them up. **Patched it** (present the whole surface on
  that path) — **no effect**, bmenace still showed `xx414`. So aborts either
  don't happen here or aren't the cause. (Note `LOG()` is a no-op with
  `C_DEBUG` off, so "Parts left"/"Lines left" never prints either way — the
  patch's null result is the real evidence, not the absent log line.)

So some pixels reach the surface without appearing in `changedLines`, and that
path is not yet found. **Cost of the safe default:** a 640x400 full frame is a
1MB write plus a whole-window reload per update. Fix this before calling the
port fast — alacritty9 measured full frames at 200-340ms vs 2-9ms for a small
delta (`alacritty9/PORT-NOTES.md`), i.e. this is THE performance item.

Note alacritty9 hit the identical wall and landed on the identical answer
(damage rects → GL9D → gl9win2 patches a persistent image), so the design is
right; our use of it has a bug.

Method note: the first A/B "proving" this compared two DIFFERENT game states
(one mid-animation, one settled) and got the right answer for the wrong reason.
The controlled version — same game, both at 45s, screen provably static — is
what actually established it. `wolf3d`'s speckle looked like a third artifact
and is not: it survives full frames, i.e. it's the authentic dithered red of
Wolf3D's own startup screen.

## Things that cost real time (read this first)

**`/dev/bintime` must be opened ONCE.** cc9's `n9_nsec()` used to
open+read+close per call. DOSBox calls `SDL_GetTicks()` every emulated tick, so
it spent more wall-clock in `open(2)` than in the emulator — DOOM's startup took
**minutes**, and `ps` showed the main proc permanently in `Open`. Fixed in
cc9 (`n9libc.c`), **measured 75.7x**: 291.81us → 3.85us per call
(`cc9/test/bintime_test.c`). The subtlety: a cached fd MUST `pread` at an
explicit **offset 0** — the old code passed -1 ("use the file offset"), which on
a reused fd walks forward and returns EOF from the second call on. One
`clock_gettime()` looks fine either way; only a loop catches it.

**`SDL_*_DISABLED` is not "report zero devices".** DOSBox calls
`SDL_Init(...|SDL_INIT_CDROM|SDL_INIT_JOYSTICK)`. `SDL_CDROM_DISABLED` makes
that whole `SDL_Init` **fail** ("SDL not built with cdrom support"). Leave the
subsystems on and let SDL's dummy backends answer honestly.

**stdout is the frame stream.** `GFX_ShowMsg` does `printf` → that corrupts
GL9F framing and kills the window. `build-dosbox.sh` patches it to stderr.
Any new `printf` in DOSBox is a landmine.

**Backends redefine `_THIS`.** `SDL_sysvideo.h` spells it `SDL_VideoDevice
*_this` (so C++ TUs can include it); every backend's private header re-defines
it to plain `this`. Ours must too.

**Register the bootstrap on an unconditional anchor.** SDL's `extern
VideoBootStrap X_bootstrap;` decls all sit inside `#if SDL_VIDEO_DRIVER_X`
guards. Inserting ours next to `DUMMY_bootstrap` put it inside a disabled
guard, where it silently compiled away. `build-sdl.sh` anchors on the
`/* This is the current video device */` comment instead.

## cc9 gaps this port found (all fixed in cc9, not shimmed here)

| gap | fix |
|-----|-----|
| `<memory.h>` | SVID alias for `<string.h>`; `cpu.cpp` + MAME cores include it unguarded |
| `<libgen.h>` | real `dirname`/`basename` + `cc9/test/libgen_test.c` |
| `execlp` | varargs wrapper over the existing `execvp` |
| `putenv` | splits and `setenv`s (it *copies* — Plan 9's env is `/env` files, so POSIX's no-copy rule is unimplementable) |
| `pthread_kill` | sig 0 = exists, 9 = "kill" note; else EINVAL. Guards the pid-reuse hazard |
| `pthread_attr_setdetachstate` | attr now really carries detachstate and `pthread_create` honors it |
| `/dev/bintime` per-call open | cached fd (75.7x) |

## Deliberately off

`C_DYNREC`/`C_DYNAMIC_X86` (needs W^X **and** a real `mprotect` — cc9's is a
no-op, so `C_HAVE_MPROTECT` must stay off or DOSBox believes protection worked),
`C_FPU_X86` (x87 inline asm), `C_OPENGL`, `C_IPX`, `C_MODEM`, `C_SSHOT`,
`libs/zmbv` (zlib). The interpreter core is why game startup is tens of
seconds; dynrec is the upgrade path.

`-Wno-register` is needed for exactly one file (`render_templates_sai.h`).
`grep -w register` finds 575 hits but they're almost all VGA *comments*.

## Dev loop

- Transfer host→guest: `python3 -m http.server` + `hget`. **Not**
  `cc9/host/deliver.py` — it embeds the binary as a C byte array and compiles it
  on the box (~24MB of C for a 3.5MB a.out).
- Transfer guest→host: `printf 'cat /path\n' | nc -w40 $VM 1717 > file` —
  **listen1 passes binary out intact** (md5-verified); the known mangling is
  inbound only.
- Launch: `rc /tmp/d9/rungame.rc <game>` mounts a fresh rio window
  (`/srv/rio.glenda.NN` — the number is per-boot) and runs the game.
  It also keeps the window raised in a loop: the listener's own terminal keeps
  printing and otherwise ends up over the game in screendumps.
- Visual check: `qmp.py screendump` → `sips -s format png`.
- macOS `tar` writes AppleDouble `._*` files into the DOS tree —
  `COPYFILE_DISABLE=1`.

## Games

`fetch-games.py` pulls shareware/freeware from archive.org; `prep-games.sh`
extracts (cracking the DEICE/INSTALL floppy blobs with `7z` rather than running
a DOS installer — they're zips behind a small header); `gen-confs.py` writes one
conf per game. **Check what you actually downloaded**: archive.org mixes
shareware with abandonware (the `wolfenstein-3d` item is the *registered* .WL6
game; `w3d-box` is the shareware .WL1 one).
