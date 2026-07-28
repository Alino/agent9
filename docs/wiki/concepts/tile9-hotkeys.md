---
title: tile9 — Global Hotkeys and Tiling
created: 2026-07-28
updated: 2026-07-28
type: concept
tags: [plan9, rio, wm, arch, ipc]
status: done
---

# tile9 — Global Hotkeys and Tiling

`src/tile9/` is a ~250-line tiling helper that ports a macOS yabai +
Karabiner window-management setup to the agent9 desktop. It is not a window
manager: it is a client of [[mxio-design]] that reads keys and writes
geometry.

| Chord | Action | yabai equivalent |
|---|---|---|
| Ctrl+Alt+Left / j | left half | `--grid 1:2:0:0:1:1` |
| Ctrl+Alt+Right / l | right half | `--grid 1:2:1:0:1:1` |
| Ctrl+Alt+Up | top half | `--grid 2:1:0:0:1:1` |
| Ctrl+Alt+Down / k | bottom half | `--grid 2:1:0:1:1:1` |
| Ctrl+Alt+Enter | maximize (not fullscreen) | `--grid 1:1:0:0:1:1` |
| Alt+1..6 | launch an app | `open -a ...` |

Padding 10px on every edge and a 10px inter-window gap — the same numbers as
the source `.yabairc`. The strip reserved along the bottom for xena-panel is
*discovered*, not hardcoded: tile9 looks for a window labelled `xena-panel`
sitting on the bottom edge and reserves its height, so the same binary tiles
correctly on the agent9 desktop (reserve 30) and on a stock rio with no
taskbar (reserve 0). It is rechecked per keystroke because riostart starts
tile9 before it creates the panel window.
Alt+digit commands default to what the image ships and are overridable in
`$home/lib/tile9` (`<digit> <rc command>` per line).

## How global hotkeys are possible at all

Plan 9 delivers keys only to the focused window, so a normal program cannot
see a system-wide chord. Rio (and therefore mxio) solves this with
**`/dev/kbdtap`**: open it `ORDWR` and rio routes every keyboard message to
you and delivers only what you write back. Acting on a chord and not writing
it back *is* the swallow. Only one tap may be open at a time
(`Einuse` otherwise), so tile9 is implicitly single-instance.

Message forms, all NUL-terminated, one per `read`:

- `k<runes>` — the full set of keys now held (sent on every press)
- `K<runes>` — the same set, after a release
- `c<rune>` — the character the press produced
- `z<id>` — the focused window changed to `<id>`

## Three traps this cost real debugging time

**1. `c` is the control-mapped rune, not the key.** With Ctrl held, the
character event for `k` is `0x0B`, for `l` it is `0x0C`, and arrows arrive as
`0xF800`. Matching the character against the chord's key rune therefore never
fires, and the control character leaks into the focused window — visible as
`\v\f` appearing in an rc prompt. Decide the swallow from *"we just fired"*
instead. rio always emits `k` before `c`, so a flag set when the chord fires
and cleared on the next key-state message is exact, and stays armed through
autorepeat.

**2. `z` is edge-triggered *and* remembered across tap opens.** mxio's
`keyboardtap` only sends `z` when `input != cur`, and `cur` is mxio state
that survives a tap closing and reopening. A freshly started tile9 attaching
to an already-focused desktop therefore never receives one and has no idea
what is focused. Never rely on `z` for the initial value — scan
`/dev/wsys/*/wctl` for the entry that is `current` and not `notcurrent`
(the same scan xena-panel uses).

**3. Ctrl+I is TAB, and mxio eats Alt+Tab before the tap.** mxio's
`keyboardtap` intercepts `c`+`\t` with alt held for its Alt+Tab focus cycle,
*before* forwarding to the tap. So Ctrl+Alt+I never reaches tile9 as a
usable chord — it alt-tabs, and the resize that tile9 does issue on the
preceding `k` message is lost in the focus change. That is why the j/k/l
layer has no `i`; use Ctrl+Alt+Up. A one-line mxio fix (require alt *without*
ctrl for the Alt+Tab branch) would free the key if it is ever wanted.

## Geometry

`resize -r x0 y0 x1 y1` written to `/dev/wsys/<id>/wctl`. Screen bounds come
from the 5×12-byte image header at the front of `/dev/screen`. Grid cells
divide the padded work area, and the last row/column absorbs the
integer-division slack so tiles meet the screen edge exactly — matching how
yabai lands on the same pixels.

Note that reading a wctl blocks forever on the second read and a hung reader
makes later opens fail `file in use`; open, one `read`, close.

## Testing

`tile9 -t` asserts eight grid cases (both reserve values) against a 1024×768 screen and exits —
runnable on-box, no window system needed. `tile9 -d` dumps every tap message
and every wctl write, which is the only practical way to see what rio is
actually sending. Live verification drives QEMU HMP `sendkey` and reads the
target window's wctl back; see [[testing-harness]].

On bare metal there is no `sendkey`. Inject through **`/dev/kbdin`** instead:
kbdfs accepts NUL-terminated `r<rune>` (down) and `R<rune>` (up) messages, so
pressing runes in order and releasing in reverse synthesises a real chord —
key-state `k` messages included, which the "old format" (a plain unterminated
string) does NOT produce. Useful runes: Kctl `F017`, Kalt `F015`, Kleft
`F011`, Kright `F012`, Kup `F00E`, Kdown `F800` (that one is `Spec|0x00`, not
in the `KF|` block).

## Wiring

Started from `riostart`; it needs `/dev/kbdtap` and `/dev/wsys`, so it must
run inside the WM namespace. A relay/listen1 shell is NOT in that namespace —
`mount /srv/rio.$user.$pid /n/aw <winid>; bind -b /n/aw /dev` first, or the
open just fails with "file does not exist". This is the usual reason tile9
"does not work" on a box: it was never started inside the window system, or
never built there at all.
