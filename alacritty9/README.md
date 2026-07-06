# alacritty9 — Alacritty on 9front

Real upstream [Alacritty](https://github.com/alacritty/alacritty) 0.17.0 — the
GPU-accelerated terminal emulator — running on stock 9front/amd64, in a rio
window, on bare metal.

The whole modern-desktop-app stack is in one static Plan 9 a.out (18.9 MB):
Alacritty + winit (with a new Plan 9 platform backend) + a glutin-shaped EGL
layer + Mesa 24 softpipe (real OpenGL 3.3, from [gl9](../gl9/)) + pure-Rust
font rasterization (fontdue + bundled Go Mono), cross-compiled with
[rust9](../rust9/) over the [cc9](../cc9/) runtime.

## Install (on 9front)

    pac9 install alacritty9

then, from a rio window:

    alacritty9

Everything works the way you'd hope: 24-bit color, 256-color, text attributes,
selection → /dev/snarf, ctrl+shift+v paste, live window resize with reflow,
^C interrupts (as a Plan 9 note to the shell's note group), `alacritty -e cmd`,
`$home/lib/alacritty/alacritty.toml` config. Full-window render+swap is ~13 ms
on a small fanless box (softpipe, no GPU) — snappier than it has any right to be.

## Architecture

Two processes, three fds (the cc9 ABI wall means the SysV-world binary can't
link kencc libdraw — same seam as gl9):

    rio window
      └─ gl9win2 (native kencc/libdraw: owns the window, reads raw /dev/kbd
         + /dev/mouse, tracks modifiers, forwards 16-byte event records)
           ├─ fd 0 → alacritty: events (key/mouse/resize/focus/quit)
           ├─ fd 1 ← alacritty: "GL9F" RGBA frames (from eglSwapBuffers)
           └─ fd 2 ← stderr passthrough

    alacritty (cc9-world a.out)
      ├─ vendor/winit platform_impl/plan9: stdin-reader thread → mpsc pump
      ├─ shim/glutin: glutin 0.32 API over gl9egl's static EGL symbols
      ├─ gl9: Mesa softpipe (GALLIUM_NOSSE=1 — no JIT on stock 9front)
      ├─ shim/crossfont: fontdue + Go Mono (4 faces compiled in)
      ├─ shim/polling: Mutex+Condvar readiness (no poll/select on Plan 9)
      └─ tty/plan9.rs: rc -i on pipes (the 9term model) + a minimal line
         discipline (ECHO, erase, ICRNL/ONLCR, ^C → note), stdout/stderr
         merged in-child via rc >[2=1], own note group via rfork s

Protocol details: [PROTOCOL.md](PROTOCOL.md). Port decisions and honest
limits: [PORT-NOTES.md](PORT-NOTES.md).

## Layout

    vendor/alacritty/   upstream v0.17.0, patched (plan9 cfg arms + tty/plan9.rs)
    vendor/winit/       upstream v0.30.13 + src/platform_impl/plan9/
    shim/{polling,glutin,crossfont,home}/   [patch.crates-io] replacements
    win/gl9win2.c       interactive window host (native, mk on-box)
    test/               headless (P1 gate), echoev.c (P2), glclear (P3)
    release/            launcher, tarball build, prebuilt gl9win2
    fetch.sh            re-pin vendored trees (records tags in vendor/PINS)

## Build (on the Mac host)

    # once: rust9 toolchain (see rust9/README) + gl9 built (_out/libgl9mesa.a)
    cd vendor/alacritty
    cargo +nightly build --release -p alacritty --no-default-features
    # gl9win2: mk in win/ on a 9front box
    # tarball: release/make-tarball.sh

The gates, each live-verified (qemu VM + bare-metal cirno):
P1 headless alacritty_terminal drives rc; P2 gl9win2 keystroke→frame round
trip; P3 winit+glutin+EGL glclear with resize + 13 ms frames; P5 the full
terminal with colors/snarf/^C/resize on hardware.
