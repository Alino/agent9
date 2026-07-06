# alacritty9 port notes

Pins: alacritty v0.17.0, winit v0.30.13 (vendor/PINS; re-pin with fetch.sh —
it OVERWRITES vendor/, our patches live in git).

## What was patched where

### vendor/alacritty (upstream v0.17.0)
- `Cargo.toml` (workspace root): `[patch.crates-io]` → shim/{polling,home,
  crossfont,glutin} + vendor/winit; profiles: panic=abort, lto off.
- `alacritty_terminal/Cargo.toml`: `libc` moved to cfg(unix) table.
- `alacritty_terminal/src/tty/plan9.rs` (NEW): the whole PTY story —
  rc on pipes via `rc -c 'rfork s; exec <shell> >[2=1]'`, one blocking reader
  thread per pipe feeding a shared buffer (sticky readiness into the polling
  shim, cleared under the buffer lock), stdout-EOF = child exit (stderr pipe
  EOFs at exec — must not signal), LineDiscipline (ECHO/erase/ICRNL/^C→
  /proc/pid/notepg "interrupt") and ONLCR in the reader. The `rfork s` is
  load-bearing: without it the interrupt notepg kills Alacritty itself
  (children share the parent note group on Plan 9).
- `alacritty/Cargo.toml`: ahash default-features off (runtime-rng → getrandom
  has no plan9), libc/signal-hook → cfg(unix); notify/tempfile →
  cfg(any(unix,windows)); xdg excluded on plan9.
- `alacritty/src/` cfg patches (~15 sites): daemon.rs (plan9 spawn_daemon +
  foreground_process_path via /proc/n/fd first line + `pub type RawFd`),
  event.rs / window_context.rs (RawFd import, master_fd = -1), config/mod.rs
  (installed_config: $home/lib/alacritty, $home/.config/alacritty,
  /lib/alacritty), config/monitor_plan9.rs (stub — no file notification on
  Plan 9), migrate (fs::rename instead of tempfile), logging.rs (console →
  STDERR: **stdout is the frame stream**, one log line = "bad frame magic"),
  clipboard.rs (SnarfClipboard for clipboard AND selection), main.rs /
  display/window.rs (startup_notify/wayland cfg exclusions).

### vendor/winit
- `src/platform_impl/plan9/` (NEW, ~1200 lines): protocol.rs (16-byte record
  parser, PROTOCOL.md), event_loop.rs (stdin reader thread → one mpsc pump;
  Poll/Wait/WaitUntil = try_recv/recv/recv_timeout; rune→Key map incl. Plan 9
  K-runes, Kdown=0x80; ModifiersChanged from gl9win2's mask), window.rs (one
  window, size from resize records), mod.rs (orbital-shaped inventory).
- build.rs cfg_aliases + platform_impl dispatch + compile_error list.
- plan9 added to `scancode` + `modifier_supplement` platform gates — upstream
  alacritty imports these unconditionally (it wouldn't build for redox either).
- RawWindowHandle: reuses the Orbital variant (nothing on plan9 inspects it).

### shims ([patch.crates-io])
- polling 3.11: Poller = Mutex<sources+notified> + Condvar; sticky
  level-triggered readable per key, writable = "write interest is always
  ready" (blocking pipe writes). `shim_*` extension methods used by tty/plan9.
- glutin 0.32.3: the exact API surface alacritty touches, over 13 statically
  linked gl9egl entrypoints + `gl9egl_surface_resize` (added to gl9egl.c —
  reallocs the OSMesa buffer and rebinds the current context). No damage
  feature reported → alacritty takes the plain swap path.
- crossfont 0.8.1: fontdue + 4 Go Mono TTFs (BSD-3, bundled). Bearings:
  left=xmin, top=ymin+height (fontdue is y-up from baseline). Underline/
  strikeout are heuristics (ponytail: swash if placement ever matters).
- home: $home (Plan 9 spelling) then $HOME.

### gl9 (small additions)
- `gl9egl.c`: gl9egl_surface_resize + current-context tracking.
- gl9win stays untouched; the interactive host is alacritty9/win/gl9win2.c.

## gl9win2 input rules (the subtle part)
Raw /dev/kbd gives 'k'/'K' (down-set diffs, base runes) and 'c' (composed
chars, auto-repeat). Emission: modifiers always from k/K; with ctrl/alt held,
base runes from k/K (so alacritty sees ctrl+shift+V as V+mods and can hit
bindings — the 'c' control byte is suppressed); everything else (plain typing,
specials ≥0xF000 and 0x80) from 'c' as down+up pairs. Mouse: /dev/mouse 'm'
lines (buttons 1/2/4, scroll 8/16 on transition), 'r' = resize → getwindow
under a qlock shared with the frame blitter.

## Honest limits (deliberate, documented)
- No raw mode: a piped child can't request /dev/consctl rawon, so full-screen
  Plan 9 apps (vi, sam -d curses-style) get canonical mode only. rc, cat,
  pipelines, compilers etc. are the use case — same as 9term for hostile apps.
- ^C interrupts the shell's note group; the shell survives (rc re-prompts).
  ^D/EOF, job control beyond that: not a thing on Plan 9.
- Window title (GL9T) is wired in the protocol + gl9win2 (/dev/label) but
  winit set_title is a no-op (fd 1 belongs to the EGL frame writer; needs a
  shared writer lock — do it if anyone cares).
- Alt is compose on Plan 9 keyboards; don't expect Alt-as-modifier bindings.
- physical_key is always Unidentified — bindings must use logical keys
  (alacritty defaults do).
- One window per process (gl9win2 owns exactly one rio window); alacritty
  CreateWindow for a second window fails with a logged error.
- Live config reload disabled (no file-change notification on Plan 9).
- atime/fsync-class fs caveats inherited from rust9 (see rust9 notes).

## Verification trail (2026-07-06)
- P1: test/headless on the qemu VM — grid contains rc output (GATE1-OK).
- P2: gl9win2 + test/echoev.c — QMP keystroke changes frame color, wctl
  resize → correct-size frames, screendumps.
- P3: test/glclear through winit+glutin+gl9egl — window/input/resize live;
  softpipe advertises dual-source blending → Glsl3 renderer path confirmed.
- P5 (VM): interactive rc in Alacritty — colors.txt (16/256/truecolor +
  attrs) pixel-checked, backspace editing, `ls / | wc -l`, ^C during sleep
  (after the rfork s fix), snarf round trip via ctrl+shift+v, live resize
  with reflow.
- P5 (bare-metal cirno): same binary + on-box mk gl9win2 — demo window
  screendumped via rio /dev/screen; glclear frame render+swap **13.2 ms** at
  712×512 (vs 40–79 ms under qemu TCG).
- Debug war stories: alacritty logging to stdout corrupted the frame stream
  ("bad frame magic"); the notepg-kills-everyone note-group bug; the rc
  null-list-in-concatenation aborting a demo script (`^`{cat missing-file}`).
