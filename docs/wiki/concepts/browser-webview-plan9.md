---
title: Browser & WebView on Plan 9
created: 2026-05-16
updated: 2026-05-16
type: concept
tags: [plan9, 9p, arch, toolchain]
---

# Browser & WebView on Plan 9

## Motivation

Shipping a real browser engine + a libwebview-compatible embedding API on 9front would
unlock two things simultaneously:

- A modern web browser for daily use on 9front
- A runtime for [[zero-native]] apps — Zig + WebView desktop apps whose UI layer is
  HTML/CSS/JS, exactly as zero-native targets macOS/Linux today

This transforms 9front from a hobbyist OS into something genuinely usable for
web-connected work.

## zero-native

zero-native (https://zero-native.dev) is a Zig framework: native binary, thin bridge
layer, WebView for UI. Sub-megabyte binaries, no bundled runtime. API is:

    zero-native init my_app --frontend svelte
    zig build run   # opens a native window with your HTML UI

The rendering stack is: zero-native → WebView → { WKWebView | WebKitGTK | CEF }.
None of those exist on Plan 9 yet. The goal of this effort is to provide a Plan 9
WebView backend so zero-native apps run on 9front unchanged.

## Engine Candidates Evaluated

| Engine         | Lines  | Deps                        | Plan 9 verdict |
|----------------|--------|-----------------------------|----------------|
| Chromium/CEF   | ~35M   | Mojo IPC, epoll, pthreads   | Non-starter    |
| Gecko/Firefox  | ~20M   | Rust (no plan9 target)      | Non-starter    |
| WPE WebKit     | ~10M   | GLib, GObject, ICU, C++17   | Hard, 2+ years |
| NetSurf        | ~500K  | Nearly none — libdraw native| Already runs   |

### WPE WebKit — why not the obvious choice

WPE was designed for embedded/non-desktop platforms (Raspberry Pi, automotive).
Its platform rendering backend is pluggable — you write a backend that pushes pixels
your way instead of OpenGL. Architecturally this is the right design for a Plan 9 port.

Blockers:
- GLib/GObject — a Linux object system with no Plan 9 equivalent; would need a large shim
- C++17 — Plan 9's GCC port is old; Clang would need to be brought up first
- ICU — enormous Unicode library, very Linux-centric build system
- JavaScriptCore JIT — architecture-specific assembly for each ISA

Net effect: you're porting a slice of Linux before you port WebKit. Estimated 2+ years
before a page renders.

### NetSurf — the existing reference

NetSurf (https://www.netsurf-browser.org) already runs on Plan 9 with a native libdraw
frontend. C codebase. Its sub-libraries are standalone and well-tested:

- **Hubbub** — HTML5 parser, validated against html5lib tests
- **LibCSS** — CSS parser + cascade engine, own test suite

No JavaScript. But it proves the libdraw rendering path is viable and provides
real working code to read.

## Target Stack — Build from the Bottom Up

    ┌─────────────────────────────────────────────┐
    │  zero-native app (Zig)                      │
    ├─────────────────────────────────────────────┤
    │  libwebview-compatible embedding API        │  ← Phase 5
    ├─────────────────────────────────────────────┤
    │  /dev/draw renderer                         │  ← Phase 4 (NetSurf reference)
    ├─────────────────────────────────────────────┤
    │  Layout engine (block, flex, text)          │  ← Phase 3 (hardest)
    ├─────────────────────────────────────────────┤
    │  HTML parser (Hubbub)  CSS engine (LibCSS)  │  ← Phase 2
    ├─────────────────────────────────────────────┤
    │  JS engine (QuickJS)                        │  ← Phase 1
    ├─────────────────────────────────────────────┤
    │  Plan 9 networking (already excellent)      │  ← already done
    └─────────────────────────────────────────────┘

## Phased Plan

### Phase 1 — QuickJS on Plan 9 (weeks)
QuickJS: Fabrice Bellard, ~200K lines C, no external deps. APE gives enough POSIX.
Validation: run QuickJS test262 suite inside 9front VM.

### Phase 2 — HTML + CSS parsing (1-2 months)
Adopt NetSurf's Hubbub and LibCSS rather than rewriting.
Validation: html5lib test suite, CSS parsing conformance tests.

### Phase 3 — Layout engine (3-6 months)
Block model, flexbox, text layout. This is the hardest layer.
No small clean reference exists. Ladybird (browser from scratch, C++) is the closest
model for how to approach it test-first.
Validation: WPT (web-platform-tests) subset — CSS2 box model, basic DOM.

### Phase 4 — /dev/draw renderer (1-2 months)
NetSurf's Plan 9 frontend is the direct reference. Extend to cover new layout output.
Validation: visual regression screenshots vs reference browser.

### Phase 5 — libwebview embedding API (weeks)
Thin C API matching what zero-native expects. zero-native's Zig bridge calls it.
Validation: zero-native "hello world" and a Svelte app running on 9front.

### Phase 6 — Upstream contribution
Contribute the Plan 9 backend to zero-native upstream.

**Total estimate:** 12-18 months focused work for one person fluent in Plan 9.

## Test-Driven Approach

web-platform-tests (https://github.com/web-platform-tests/wpt) is the canonical
W3C test suite — ~50K tests, used by Chrome, Firefox, Safari, Ladybird.
Pick a conformance target (e.g. HTML5 parser + CSS2 + ES2020 subset) and treat
passing the relevant WPT slice as the definition of done per phase.

This is exactly how Ladybird and NetSurf validated their work — implement to spec,
test against the same suite everyone else uses.

## Why Plan 9 is Actually a Good Host

- Everything is a file: networking, devices, IPC — browser can speak 9P natively
- No shared-memory races from competing display servers
- Clean process model via rfork — natural fit for isolating renderer processes
- libdraw is a simple, auditable pixel-pushing API — no GPU driver complexity

A browser that speaks 9P natively instead of POSIX sockets would be genuinely novel
and more architecturally coherent than any existing port.

## Relation to plan9-winxp

This is a **separate project** from the WinXP UI shell. mxio/xena-panel/launcher/xfiles
are all pure libdraw C and do not need a browser engine.

The browser effort would live in its own repo (e.g. `plan9-browser` or `p9webview`).
If it reaches Phase 5, zero-native apps become a new category of 9front software
that can be launched via the [[launcher]] component of plan9-winxp.

## See Also

- [[draw-api]] — how /dev/draw works (the renderer target)
- [[build-toolchain]] — cross-compile setup, relevant for QuickJS Phase 1
- [[testing-harness]] — QMP + screendump loop, usable for visual regression in Phase 4
