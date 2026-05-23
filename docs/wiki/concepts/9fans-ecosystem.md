---
title: 9fans GitHub Ecosystem
created: 2026-05-16
updated: 2026-05-16
type: reference
tags: [plan9, libdraw, toolchain, 9p, arch]
---

# 9fans GitHub Ecosystem

The https://github.com/9fans organization hosts 8 public repos, all
MIT licensed. Their relevance to agent9 and [[browser-webview-plan9]]
is sorted below.

---

## plan9port — Plan 9 from User Space

https://github.com/9fans/plan9port
**Stars: 1.9k | Language: C (91%) | Maintainer: Russ Cox | Active: 2026**

Plan 9 tools ported to Unix/macOS. The foundation for cross-platform
testing.

### What's relevant

| Component | Use in agent9 |
|-----------|---------------|
| `libdraw` | Source reference (MIT) — read it any time you need to look at how something works at a deeper level. |
| `libmemdraw` | **Critical for the browser renderer** — in-memory drawing without /dev/draw; render to a bitmap, then push to /dev/draw in one shot. |
| `libmemlayer` | Layer compositing — relevant for mxio z-order and window stacking. |
| `devdraw` | Emulates /dev/draw via Quartz/X11 → lets you test libdraw code on macOS without a VM. |
| Online man pages | https://9fans.github.io/plan9port/man/ — complete offline-free reference. |

### Online man pages

```
https://9fans.github.io/plan9port/man/man3/draw.html   -- libdraw
https://9fans.github.io/plan9port/man/man3/memdraw.html -- libmemdraw
https://9fans.github.io/plan9port/man/man3/thread.html  -- threads/channels
https://9fans.github.io/plan9port/man/man2/9p.html      -- 9P protocol
```

### License

MIT since March 2021 (Nokia transferred copyright to the Plan 9
Foundation). Fonts have an exception — see the root `LICENSE`. Code
(libdraw, libc, etc.) is straight MIT.

### Installing on a Mac

```bash
brew install plan9port
# then: 9c file.c && 9l -o file file.o
```

Limitation: graphics go through devdraw (Quartz), not the 9front
kernel. Good for testing logic; production builds have to happen in
the VM. See [[build-toolchain]].

---

## 9fans/go — Plan 9 packages for Go

https://github.com/9fans/go
**Stars: 366 | Language: Go | Active: 2025**

Go ports of Plan 9 libraries. There's no Go runtime in agent9, but
this is a **readable reference**.

### draw/

A Go port of the entire libdraw. Includes:

- `draw/memdraw/` — in-memory drawing with benchmarks; the best way
  to understand the performance characteristics of compositing
  operations.
- `draw/drawfcall/` — **the wire protocol between an app and the
  /dev/draw kernel driver** — the messages written to
  `/dev/draw/N/data`. Useful if you ever build a custom display
  server or debug the kernel-side communication.
- `draw/frame/` — text frame widget (how rio and acme render text).

The Go names map to C names in the package comments — a good way to
figure out which Go symbol corresponds to which C API.

### plan9/srv9p/

**Go port of lib9p** — a 9P file server library. Added in March 2025.

Relevant to [[xena-panel-design]]: xena-panel serves a 9P file tree
(`/mnt/taskbar/` or similar) that mxio writes window events into and
reads state from. srv9p demonstrates patterns for:

- fid management (Tattach/Twalk/Topen/Tread/Twrite/Tclunk)
- request handler structure
- posting/mounting the server

The protocol is identical in C and Go — srv9p is a readable reference
before you write the C implementation.

### plumb/

A Go plumber client (`/mnt/plumb`). Reference for [[launcher]] —
how to send plumb messages correctly to open files/URLs.

---

## drawterm — Plan 9 CPU client

https://github.com/9fans/drawterm
**Stars: 151 | Language: C | Active: 2024**

Connects to a Plan 9 CPU server from macOS/Linux/Windows. Carries
its own copies of libdraw, libmemdraw, libmemlayer.

### Why it's interesting

It has **platform backends** that translate /dev/draw to native
graphics:

```
gui-osx/    -- /dev/draw → macOS Quartz
gui-x11/    -- /dev/draw → X11
gui-win32/  -- /dev/draw → Win32 GDI
```

This is the **inverse of the browser problem**: drawterm takes
/dev/draw output and paints it onto macOS. A browser would take
layout output and paint it INTO /dev/draw. The gui-osx backend is
a direct code reference for the rendering architecture either way.

### Known TODOs (from the README)

- Import the latest /dev/draw to allow **window resizing** — mxio
  will need this too.
- Make the console window a real 9term window.
- Implement `/dev/label`.

### Building on a Mac

```bash
git clone https://github.com/9fans/drawterm
CONF=osx make
./drawterm -h <ip_of_9front_vm>
```

An alternative to VNC — a native macOS window instead of a VNC client.

---

## vx32 — Sandboxed x86 execution

https://github.com/9fans/vx32
**Stars: 132 | Language: C | Last activity: 2022**

Sandboxed x86 emulation. Includes `9vx` — a Plan 9 VM on top of vx32.

**Relevance: low.** Inactive since 2022. Historically interesting (how
to run Plan 9 in userspace) but no practical value for agent9.

---

## acme-lsp — LSP for acme

https://github.com/9fans/acme-lsp
**Language: Go | Active: 2026**

Language Server Protocol tools for the acme editor. If you edit C
code in acme inside the VM, acme-lsp + clangd gives you autocomplete
and go-to-definition.

**Relevance: dev tooling.** Not project code, but it makes working in
the VM easier.

---

## Relevance per component

| Component | Most important repos |
|-----------|----------------------|
| mxio (WM) | plan9port libmemlayer (z-order ref), drawterm gui-osx (resize ref) |
| xena-panel | 9fans/go srv9p (9P server pattern) |
| launcher | 9fans/go plumb (plumber client ref) |
| xfiles | plan9port libdraw (image loading, Font) |
| browser engine | plan9port libmemdraw (render target), drawfcall (wire protocol) |
| build/test | plan9port devdraw (Mac testing), online man pages |

---

## Related pages

- [[draw-api]] — libdraw API detail
- [[build-toolchain]] — cross-compiling and plan9port on a Mac
- [[xena-panel-design]] — 9P file service pattern
- [[browser-webview-plan9]] — browser engine plan, where libmemdraw plays the key role
