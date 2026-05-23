---
title: vt — 9P Namespace
created: 2026-05-16
updated: 2026-05-16
type: reference
tags: [arch, plan9, 9p, ipc, status-wip]
---

# vt — 9P Namespace

The on-the-wire interface for vt. Posted at `/srv/vt`. Mount with
`mount -c /srv/vt /n/vt` (or whichever mountpoint you prefer).

## Tree layout

```
/                      directory, 0555
  ctl                  rw, 0666   — daemon control
  <session>/           directory per session
    cons               rw, 0666   — bidirectional shell I/O (replaces /dev/cons)
    consctl            w,  0222   — raw mode, line-editor control
    cells              r,  0444   — binary cell-diff stream (for native clients)
    size               rw, 0666   — "rows cols\n"
    scroll             r,  0444   — UTF-8 scrollback (one row per line)
    title              rw, 0666   — window title, OSC 0/2 writes here
```

## Root /ctl commands

Plain-text, newline-terminated. Read returns daemon status.

```
new <name>             create new session named <name>, spawn rc
kill <name>            terminate the session
rename <old> <new>     rename a session
```

Errors return a 9P Rerror with descriptive text.

## /<session>/cons

Same semantics as Plan 9's `/dev/cons`:

- Writes from clients (mxio, vt-attach) deliver keystrokes to rc.
- Writes from inside the session (rc and its children) deliver output bytes,
  which vt feeds through the VT100 parser before storing in the cell buffer.
- Reads from inside the session block until keystrokes arrive.
- Reads from outside (debugging) return raw bytes after parsing — primarily
  useful for testing.

The bind that makes this work happens inside the rc child's namespace:
vt's session-spawn code rforks RFNAMEG, then `mount -b` (bind-before) the
session's cons file over /dev/cons in the child namespace only. vt's own
namespace is untouched.

## /<session>/consctl

Write-only. Newline-terminated commands.

```
rawon                  put input in raw mode (don't cook lines in kernel)
rawoff                 cooked mode
edit on                vt's in-process line editor handles input
edit off               pass keystrokes straight through
holdon                 buffer output, don't update cells (atomic redraws)
holdoff                resume cell updates
size R C               resize the cell buffer (rows, cols); marks all dirty
redraw                 mark every cell dirty without resizing — used by
                       clients on focus-gain or after suspected display drift
```

Default: `rawon` + `edit on` for interactive sessions.

**Note:** as of 2026-05-16 the ctl commands accepted by vts are
actually written to `/<session>/ctl`, not `/<session>/consctl`. The
wiki page name is historical; the file lives at `ctl`. To be cleaned
up in a future pass.

## /<session>/cells

Read-only binary stream. Each read returns the diffs that have accumulated
since the last read on this fid. Walk-clunk-rewalk forces a full repaint.

### Wire format

```
struct CellHeader {
    u32  magic;          // 0x76746331 = 'vtc1', little-endian
    u16  version;        // 1
    u16  rows, cols;     // current buffer dimensions
    u32  ncells;         // number of cell changes in this frame
    u16  cursor_row;
    u16  cursor_col;
    u8   cursor_visible; // 1 or 0
    u8   reserved[3];
};

struct Cell {
    u16  row, col;
    u32  rune;           // UTF-32 code point
    u8   fg, bg;         // 16-color palette indices (0-15)
    u8   attrs;          // bit0 bold, bit1 underline, bit2 reverse
    u8   reserved;
};
```

All u16 / u32 are little-endian. One frame = one header + ncells * Cell.

### Why u32 ncells

A `clear` followed by full-screen redraw can touch every cell. With 200x80
that's 16000 cells × 10 bytes = 160 KB per frame. We coalesce: cells written
multiple times within the batching window are deduped (last write wins).

## /<session>/size

Plain text: `"rows cols\n"`. Read returns current size, write resizes.
On write, vt resizes the cell buffer (preserving content where possible) and
sends a window-change note to rc via `postnote(PNGROUP, pid, "sys: window
size change")`.

## /<session>/scroll

UTF-8, one buffer row per line, newest at the bottom. Reads return the entire
scrollback (default 10000 lines). Useful for paging through history without
needing the cell protocol.

A future `/<session>/scroll.raw` could expose the same data with attributes;
not in v1.

## /<session>/title

Plain text. Read returns current title. Writes from inside the session (via
OSC 0 or OSC 2 in the parser) update it; external writes work too (e.g. for
custom prompts that set the title separately).

## Concurrency model

vt uses libthread; one proc per session for the parser, one for the line
editor. lib9p's `threadsrv` dispatches 9P requests on a separate proc. All
share access to the cell buffer via a per-session `QLock`.

## See Also
- [[vt-architecture]] — overview
- [[vt100-parsing]] — what bytes turn into what cells
