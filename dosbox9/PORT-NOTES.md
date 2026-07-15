# dosbox9 — DOSBox 0.74-3 on 9front via cc9

**Status: DOSBox runs on 9front. 12/12 games launch and render** on bare-metal
cirno — Doom, Wolfenstein 3D, Commander Keen 1 & 4, Jill of the Jungle, Crystal
Caves, Raptor, Blake Stone, Duke Nukem II, Bio Menace, Halloween Harry, Hocus
Pocus. Boots to `Z:\>`; keyboard works end-to-end (`dir` lists the drive).
**Windows 3.11 for Workgroups runs with its full GUI**, and the Windows 98 SE
boot floppy reaches an interactive `A:\>` (real mode only — see below).
Screenshots in `screenshots/` (`host/shoot-games.sh` regenerates the games).

Damage rects (`GL9D`) are the default and pixel-correct as of 2026-07-15;
`P9_FULLFRAMES=1` forces whole frames. The long-standing "delta bug" was an
alpha blend — see below.

Not yet: **sound is untested** (the VM has no `/dev/audio`; cirno has one and
the write path runs, but nobody has listened). Nothing has been played for more
than a title screen.

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

## FIXED: the "GL9D delta bug" was an alpha blend

**Deltas are the default now.** `P9_FULLFRAMES=1` opts back into full frames.

The symptom: with deltas, `bmenace`/`keen4` showed old text under new — the
memory counter read `xx414`, the status box `Loaddyg..PresPleaseyWait` — and it
persisted on a static screen. Full frames were clean.

**Root cause: we shipped pixels with alpha 0 into an image that has an alpha
channel.** gl9win2 allocates its Image as `ABGR32`, so Plan 9's `draw(2)`
computes

    dst = src + dst*(1 - src.alpha)

from our 4th byte. Our SDL masks declare no alpha (`Amask=0`), so `SDL_MapRGB`
left it **0** — and every draw became an **additive blend** instead of a
replace. Hence "old text under new" and colours creeping brighter.

**Why full frames looked innocent:** gl9win2 blacks the window before drawing a
GL9F, and `src + 0 == src`. So the full-frame path was correct *by accident*,
and the delta path — which draws straight onto live pixels — took the blame.
That asymmetry is what made this so hard to see: every A/B said "deltas bad,
full frames good", which is true but points at the wrong layer.

**Why alacritty9 never hit it:** OSMesa already writes alpha 255.

The fix is 6 lines in `P9_UpdateRects` — OR `0xFF000000` into the staging
surface before shipping. The protocol says RGBA, so sending 0 was our bug, not
gl9win2's; gl9win2 is still used unmodified.

**`changedLines` was never guilty.** The old notes claimed "some pixels reach
the surface without appearing in `changedLines`". That is **false** and cost
three sessions. It was disproved by building a from-scratch differ that ignored
DOSBox's rects entirely and diffed consecutive frames: it picked *exactly* the
same rows (both say "13 rows at y=354" for bmenace's blinking status box), and
the artifact was **unchanged**. DOSBox's rects are honest. That differ was then
deleted — it was solving a problem that did not exist.

Also disproved along the way, each by measurement:

- *Palette changes are dropped* (`Check_Palette` clears `render.pal.modified[]`
  one frame after publishing it, and `RENDER_StartUpdate` can bail after that
  via `!GFX_StartUpdate`). Plausible and wrong: instrumented both unsafe exits,
  **zero** hits in delta and full runs alike.
- *`RENDER_EndUpdate(abort=true)` loses writes.* Patched to present the whole
  surface on that path — **no effect**.
- *gl9win2 drops out-of-range GL9D / `loadimage` short-loads.* Instrumented:
  zero hits, zero failures. (Both true — the data always arrived; it was
  blended wrong on arrival.)

**The lesson worth keeping:** every one of those measurements was *correct*.
They all interrogated whether the right *data* arrived, and it always did. The
bug was in how the data was *combined* with what was already there. When "the
bytes are provably right but the screen is wrong", stop auditing the pipe and
start auditing the raster op.

Method note: the decisive experiment was dumping the staging surface and the
screen **at the same instant** (`P9_DUMP_FRAME=1` + `touch /tmp/d9/DUMP`, then
`shot.rc`). That showed a clean frame against a dirty window and killed every
"lost update" theory at once. The earlier one-shot dump was useless because you
cannot know the interesting update's number in advance. Also: `wolf3d`'s speckle
is not an artifact — it survives full frames, i.e. it's the authentic dithered
red of Wolf3D's own startup screen.

## Windows

**Windows 3.11 for Workgroups runs, GUI and all** (`screenshots/win311-desktop.png`):
Program Manager, groups, mouse. It needs no `boot` — `WIN.COM` is a plain DOS
program, so it's `mount c <tree>` + `win` (inline in `README.md`). Preinstalled tree
is the archive.org `install-me` item's `WINDOWS.zip` (~15MB extracted). **This is
the "Windows on 9front" shot** — reach for it, not Win98.

**Windows 98 boots only to real mode** (`screenshots/win98.png`): the SE boot
floppy via `BOOT.COM`, real `IO.SYS` + `COMMAND.COM`, `VER` says
`Windows 98 [Version 4.10.2222]`, interactive `A:\>`, and the `CONFIG.SYS`
real-mode driver stack (Oak ATAPI, Adaptec ASPI, PCI scan) loads. Image is the
archive.org `win-98-se-boot-disk` item; conf in `README.md`.

**The Win98 GUI does not run and won't** — vanilla DOSBox 0.74 has never
supported Win9x (no protected-mode/V86 + IDE emulation for `VMM32.VXD`). That's
an upstream limit, not ours; Win98-in-DOSBox means **DOSBox-X**. Don't burn time
here without porting DOSBox-X first.

Traps:

- **8.3 names.** `boot c:\win98boot.img` fails with "Bootdisk file does not
  exist" — 11 chars. Rename to `win98.img`.
- **`cputype=pentium` is not a value** in 0.74 (it's `pentium_slow`); the
  invalid one silently resets to `auto` and only says so in the log.
- **Don't mount a FAT12 image on macOS to patch it** — the OS writes
  `System Volume Information` / Spotlight metadata into it. Use `mtools`
  (`mcopy -i img -o file ::/FILE`), which touches nothing else.

## Things that cost real time (read this first)

**Another agent's http.server can steal your port.** A concurrent session had
`python3 -m http.server 8801` bound to **`192.168.88.10:8801`** (IPv4, specific
address) while mine was on `*:8801` (IPv6 wildcard). The specific bind wins for
LAN traffic, so cirno's `hget` silently fetched a **404 page** and wrote a
**zero-byte** `win98.img` — which boots to a cursor at "Booting from drive A..."
and looks exactly like a corrupted image. I "proved" hdiutil and then mtools
were corrupting the floppy before checking. **`md5sum` the file on the guest
against the host after every transfer** — `hget` does not fail loudly. See
[[agent9-concurrent-sessions]].

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

**An idempotent patcher must delete the same unit it inserts.**
`build-sdl.sh`'s `register()` inserted a three-line block
(`#if`/`extern`/`#endif`) but de-duplicated by filtering out only the *extern
line*. So each build left the `#if`/`#endif` husk and appended a fresh block —
and since `vendor/SDL-1.2.15` is tracked, the junk accumulated in git (4 husks
were committed before anyone noticed; a session that builds six times adds six
more). Fixed 2026-07-15: remove the whole block, plus a regex sweep for orphaned
guards. Check it stays fixed by running `build-sdl.sh` twice and diffing — two
consecutive runs must produce identical trees.

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
