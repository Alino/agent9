---
title: VT100 Parsing in vt
created: 2026-05-16
updated: 2026-05-16
type: reference
tags: [arch, status-wip]
---

# VT100 Parsing in vt

Defines exactly which control sequences vt understands. The parser is a
straight port of Paul Williams' VT500 state machine, the same one st uses.
Reference: <https://vt100.net/emu/dec_ansi_parser>.

## States

```
Ground                  — normal text
Escape                  — saw ESC, deciding what kind
EscapeIntermediate      — ESC then intermediate byte
CsiEntry                — ESC [
CsiParam                — collecting parameter bytes
CsiIntermediate         — collecting intermediate bytes
CsiIgnore               — unrecognized, swallowing until final byte
OscString               — ESC ] string ST
SosPmApcString          — swallowing exotic strings
DcsEntry                — ESC P, only used to skip DCS cleanly
```

Any unrecognized byte sequence returns the parser to Ground. We never lock up
on bad input.

## Implemented sequences

### C0 controls (printable: pushed to buffer; control: handled)

```
0x07  BEL    — flash visual bell (mxio client) / ignore (vt-attach)
0x08  BS     — cursor left, no wrap
0x09  HT     — tab to next 8-column stop
0x0A  LF     — newline: cursor to col 0 of next row, scroll if at bottom
0x0B  VT     — same as LF
0x0C  FF     — same as LF
0x0D  CR     — cursor x = 0
0x1B  ESC    — enter Escape state
```

All other 0x00-0x1F bytes are dropped. 0x7F (DEL) is dropped.

### ESC dispatches

```
ESC c     RIS  — full reset (clear screen, home cursor, default colors)
ESC D     IND  — index (cursor down, keep column, scroll if needed)
ESC E     NEL  — next line (CR + LF: col=0, cursor down)
ESC M     RI   — reverse index (cursor up, scroll if needed)
ESC 7     DECSC — save cursor position + attrs
ESC 8     DECRC — restore cursor position + attrs
```

## Plan 9 deviation from strict VT100: bare LF is "newline"

In strict VT100, LF is "index" — moves cursor down one row, **column
unchanged**. Programs are expected to emit CRLF (or ESC E / NEL) to get to
the start of the next line.

Plan 9 doesn't do that. `/dev/cons`, rc, sam, and everything in the system
treat `\n` as the line terminator. There's no tty line discipline turning
LF into CRLF on output. If a Plan 9 program prints `"foo\nbar\n"`, it
expects `bar` to start at column 0 — and on rio's built-in terminals it
does, because rio's text engine is line-oriented (not cell-grid) and
operates on the text stream directly.

vt is a cell-grid terminal, so it has to make the choice explicit. We
follow Plan 9 convention: **bare LF resets the cursor column to 0**.
That's what `cellbuf_newline()` does. Strict VT100 IND semantics are
preserved for `ESC D` via the separate `cellbuf_index()` entry point.

Without this, every rc command leaves a staircase of right-shifted lines
because rc emits `\n` between fields, not CRLF.

### CSI sequences

`ESC [ params intermediates final`. Parameters are decimal numbers separated
by `;`. Missing parameters default per-sequence (usually 1 or 0).

```
CSI n A    CUU  — cursor up n (default 1)
CSI n B    CUD  — cursor down n
CSI n C    CUF  — cursor right n
CSI n D    CUB  — cursor left n
CSI r;c H  CUP  — cursor to row r, col c (1-indexed)
CSI r;c f  HVP  — same as CUP
CSI n J    ED   — erase display: 0=below cursor, 1=above, 2=all
CSI n K    EL   — erase line: 0=right, 1=left, 2=whole
CSI n m    SGR  — graphics rendition (see below)
CSI ?25 h  DECTCEM — show cursor
CSI ?25 l           — hide cursor
CSI ?1049 h DECSET  — alt screen (we treat as clear+save)
CSI ?1049 l         — restore from alt screen
CSI s      SCP  — save cursor
CSI u      RCP  — restore cursor
```

### SGR — Set Graphics Rendition

`CSI n;n;n m` — each n is one of:

```
0                reset all
1                bold
4                underline
7                reverse video
22               not bold
24               not underlined
27               not reversed
30-37            foreground color 0-7
38;5;N           foreground 256-color (we truncate to 16)
39               default foreground
40-47            background color 0-7
48;5;N           background 256-color (we truncate to 16)
49               default background
90-97            foreground bright 8-15
100-107          background bright 8-15
```

True color (`38;2;R;G;B`) is parsed-and-quantized: we find the nearest of our
16 palette entries. Lossy but doesn't lock up.

### OSC — Operating System Command

`ESC ] n ; text ST`. ST = `ESC \\` or `BEL` (0x07).

```
OSC 0 ;text    set window title to text
OSC 2 ;text    set window title to text
```

All other OSC commands are consumed and discarded.

## What we deliberately ignore

- Mouse reporting (`CSI ?1000 h` and friends)
- Bracketed paste (`CSI ?2004 h`)
- Application keypad mode (`ESC =` / `ESC >`)
- Origin mode (`CSI ?6 h`)
- Auto-wrap mode toggles (we always wrap)
- Sixel, ReGIS, Kitty image protocol
- DCS (passed through state machine but never dispatches)

These are consumed without effect — the parser just goes back to Ground.

## Color palette

vt ships a fixed 16-color palette tuned for the WinXP Luna desktop:

```
 0 black        #000000
 1 red          #cd3131
 2 green        #1e9c40
 3 yellow       #d8a800
 4 blue         #1f5cb7
 5 magenta      #b833a4
 6 cyan         #2cb8b8
 7 white        #cccccc
 8 bright black #5e5e5e
 9 br red       #f14848
10 br green     #2bd84d
11 br yellow    #ffd341
12 br blue      #4283d8
13 br magenta   #e054cb
14 br cyan      #4adada
15 br white     #ffffff
```

mxio clients map these to libdraw `Image*`s (one allocimage per index, reused
across the session). vt-attach emits the ANSI numbers directly — the remote
terminal's own palette interprets them.

## See Also
- [[vt-architecture]]
- [[vt-9p-namespace]]
- st reference parser: https://git.suckless.org/st/file/st.c.html (search for tputc)
