---
title: pi9 Phase 9 — DECAWM, Scrollback Nav, Install Path
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [arch, status-done, plan9]
---

# pi9 Phase 9 — DECAWM, Scrollback Nav, Install Path

> **Status: done 2026-05-17.** Three carry-overs cleared: vts now
> supports DECAWM (CSI ?7 h/l) so pi9 doesn't need the width-1
> hack; pi9 has scrollback navigation (pgup/pgdn/shift-up/shift-down/ctrl-end);
> pi9 has a proper install path at `/$cputype/bin/pi9` via
> `pi9-install` + the existing mkfile install rule.
>
> Skipped: live OpenRouter test (user didn't provide a key in time).
> The infrastructure is in place — `/login <key>` + send a message.

## What this phase fixes

Three items from the "what's not done" list in the wrap-up:

1. **No install story** (item 3) — pi9 lived at `/tmp/pi9` and got
   re-pushed on every test. Now it has a permanent home in
   `/$cputype/bin/pi9` and a one-shot installer.
2. **Scrollback navigation missing** (item 4) — `/help` overflowed,
   long tool outputs were unreachable. Now: pgup/pgdn/shift+arrow
   keys + a "scrolled N rows" status indicator.
3. **Renderer hacks remain** (item 8) — the width-1 trick was a
   patch over a vts DECAWM gap. Added DECAWM support in vts; pi9
   negotiates auto-wrap off on startup. Width-1 trick stays as
   fallback for pre-Phase-9 vts.

## DECAWM in vts

The original bug: vts auto-wraps when a printable rune lands at
column `cols-1` and would advance past. Auto-wrap calls
`cellbuf_newline`, which scrolls if the cursor is on the last row.
Pi9's last write (status bar's last cell) triggered this scroll
silently. Bubbletea's diff renderer didn't know, so the next
`CursorUp(N-1)` landed at the wrong position. Compounded per
keystroke into the "each character creates a new line" bug from
Phase 7.

The Phase 8 fix was a workaround: render only to `m.width-1` columns,
last column always blank. vts never auto-wraps. Bug avoided.

Phase 9's proper fix: vts implements DECAWM (CSI ?7 h/l). DECAWM
off means "cursor sticks at the last column on overflow, subsequent
writes overwrite that cell." Pi9 emits `\x1b[?7l` at startup, `\x1b[?7h`
on exit.

### Three small changes in vts

`cells.h`:

```c
struct Buffer {
    /* ... */
    uchar wrap;  /* DECAWM: 1=auto-wrap (default), 0=sticky last col */
};
```

`cells.c` — `cellbuf_init` sets `b->wrap = 1`. `cellbuf_advance`:

```c
b->cur_col++;
if(b->cur_col >= b->cols){
    if(b->wrap){
        b->cur_col = 0;
        cellbuf_newline(b);
    } else {
        b->cur_col = b->cols - 1;  // glue to last col
    }
}
```

`parser.c` — extend the existing DECSET/DECRST switch:

```c
case 'h':  // SM
    if(p->csi_private){
        for(i = 0; i < p->nparams; i++){
            int code = p->params[i];
            if(code == 7) b->wrap = 1;       // DECAWM on
            else if(code == 25) ...           // existing
        }
    }
case 'l':  // RM
    if(p->csi_private){
        for(...) if(code == 7) b->wrap = 0;  // DECAWM off
    }
```

That's the entire patch. ~25 lines across three files.

### Pi9's side

`main.go`, just before `p.Run()`:

```go
// DECAWM off: cursor sticks at last col instead of wrapping.
// Restored on exit.
_, _ = os.Stdout.WriteString("\x1b[?7l")
defer os.Stdout.WriteString("\x1b[?7h")
```

On a vts that supports DECAWM, this disables auto-wrap and pi9 can
freely paint into the last cell. On older vts (or any other terminal
without DECAWM), the escape is silently ignored — pi9 still works
because the width-1 trick in `View()` is still there as belt-and-suspenders.

When all running vts daemons are Phase-9-or-later, we can drop the
width-1 trick from pi9. For now it's harmless extra paranoia.

### Hot-restart not done

Updating vts requires restarting the daemon, which would kill every
running vts session (including `1`, where listen1 lives — that would
sever my own access to the VM). Plan: ship the new vts binary,
verify it builds and installs to `/amd64/bin/vts`, leave the old
vts daemon running, restart on the next VM boot.

This is the right tradeoff for a hobby project: don't lose access
debugging the change.

## Scrollback navigation

The original problem: `/help` output (40 lines after Phase 8b)
exceeded the visible scrollback area. So did any long tool output.
There was no way to scroll up.

Now there is. `pi9Model` got a `scrollOffset` field — rows scrolled
UP from the latest. View() renders `scrollback[total-offset-rows : total-offset]`
via a new `fitBlockOffset()` helper. Keys wired in `handleKey`:

| Key | Action |
|---|---|
| `pgup` (or `ctrl+b`) | scroll up by ~screen height |
| `pgdn` (or `ctrl+f`) | scroll down by ~screen height |
| `shift+up` / `alt+up` | scroll one row up |
| `shift+down` / `alt+down` | scroll one row down |
| `ctrl+end` | jump to live tail (offset=0) |

When `scrollOffset > 0` the status bar swaps from
`enter to send · /help · ctrl-c to quit` to
`scrolled N rows up - pgdn/ctrl+end to return`. So the user knows
they're not pinned anymore.

New content (streaming chunks, new turns) does NOT auto-reset
scrollOffset. The user is in control. If they scrolled up and a
stream finishes, the screen stays put — only the bottom of the
buffer changes, invisible to them until they ctrl+end.

This is deliberate. The opposite (auto-reset on new content) is
infuriating: you scroll up to read something, a chunk arrives, you
get yanked back to the bottom. Treats the scroll position as user
intent, like `less` does.

## Install path

Pi9 lived at `/tmp/pi9` because dev iterations push via `hget` and
`/tmp` is the natural scratch space. That's fine for development;
it's wrong for "this is the canonical pi9 install."

Phase 9 adds:

### `src/pi9/mkfile`

A real plan9 mkfile that does the install + uninstall side. Doesn't
attempt to build pi9 (no Go toolchain on 9front); expects
`pi9.plan9.$cputype` to already exist beside it.

```bash
# On Mac:
GOOS=plan9 GOARCH=amd64 go build -o pi9.plan9.amd64 .
# Copy mkfile + binary + new-pi9 to the VM, then:
mk install
```

Installs to `/$cputype/bin/pi9` and `/rc/bin/new-pi9`. Targets:
`install`, `uninstall`, `clean`, `nuke` (the last wipes
`$home/lib/pi9` with a 5-second confirmation pause).

### `src/pi9/pi9-install`

A one-shot installer rc script for users who just want to "make it
work." Pulls the binary + new-pi9 from a build host over HTTP,
installs them, prints helpful next-step hints.

```bash
hget http://your-host/pi9-install | rc
```

Or with a different download base:

```bash
PI9_URL=http://build.example.com pi9-install
```

Tested end-to-end:
- Pulled binary (7.6 MB) and new-pi9 from the Mac's http.server
- Installed to `/amd64/bin/pi9` + `/rc/bin/new-pi9`
- `whatis pi9` returns `/bin/pi9` (PATH resolution working)
- Start menu → Pi9 → vtwin opens with pi9 chat UI running

### Updated new-pi9

Updated `src/launcher/new-pi9` to prefer `/bin/pi9` over `/tmp/pi9`:

```rc
if(test -x /bin/pi9){
    echo pi9 > /n/vts/$sess/cons
}
if(! test -x /bin/pi9 && test -x /tmp/pi9){
    echo /tmp/pi9 > /n/vts/$sess/cons
}
if(! test -x /bin/pi9 && ! test -x /tmp/pi9){
    echo 'echo new-pi9: no pi9 binary in /bin or /tmp' > /n/vts/$sess/cons
}
```

Falls back to `/tmp` for dev convenience. Errors helpfully if neither
exists.

## Live API not yet tested

Pi9 has never actually talked to a real LLM. Everything has been
against the mock Python server. The OpenRouter SSE client code is
correct in theory but unproven against actual openrouter.ai.

To test: get an OpenRouter key at <https://openrouter.ai/keys>,
launch pi9, and run `/login sk-or-v1-...`. Then send any message.
If it works, pi9's Phase 2-3 work is fully validated.

If it fails: the most likely culprits are TLS chain validation
(plan9's `/sys/lib/tls/ca.pem` may not include the cert chain
openrouter.ai uses) or SSE parsing edge cases the mock didn't
exercise. Both fixable.

## Verified end-to-end

Screenshot: `wiki/assets/pi9-phase9-rendered.png`. Shows pi9 launched
via the start menu, running from `/bin/pi9` (the installed path).
The chat UI is clean — header in Luna blue, scrollback with prior
turns including a `/help` panel, input box rectangle, status bar.

What we did NOT verify:
- DECAWM off in practice. Requires restarting vts; we built but
  did not restart. Next VM boot will use the new vts and DECAWM
  will be active.
- Real API call. Skipped — no key provided.

## Architecture additions

```
src/vts/cells.h       + Buffer.wrap field
src/vts/cells.c       + DECAWM logic in cellbuf_advance, init=1
src/vts/parser.c      + CSI ?7 h/l handling in DECSET/DECRST

src/pi9/main.go       + scrollOffset field on pi9Model
                      + pgup/pgdn/shift+up/shift+down/ctrl+end handlers
                      + fitBlockOffset() helper
                      + "scrolled N rows" status indicator
                      + DECAWM-off escape at startup, restore at exit

src/pi9/mkfile        replaced with a real install mkfile
src/pi9/pi9-install   NEW: one-shot installer rc script
src/launcher/new-pi9  + prefers /bin/pi9 over /tmp/pi9
```

Tool count: still 11 (Phase 5). Slash command count: still 18 (Phase 8b).
This phase is plumbing.

## Quirks worth remembering

### rc parses `k=v` as assignment

`echo api_key=mock-key > file` is a SYNTAX ERROR in rc: rc interprets
`api_key=mock-key` as a variable assignment on the same line as
`echo`, and the resulting echo call has no arguments. Quote it:
`echo 'api_key=mock-key' > file`. Lost ~15 minutes to this.

### nc disconnects are noisy

Long-running rc scripts via `nc -w N ... 1717` regularly drop before
the script finishes. Things still run, but their output disappears.
Mitigation: redirect inside the script to a known file path, then
`cat` it in a separate `nc` invocation. Or, even better, write the
script with `> /tmp/done` as the last line and poll for that file.

### Plan9 file cache makes `cat` lie

After writing to a file in one rc shell (one nc connection), reading
the file in a fresh rc shell can return stale content for a few
seconds. The actual file system has the new bytes; the namespace
cache hasn't caught up. Not unique to our setup; standard cwfs
behavior. Workaround: read and write in the same shell.

### Updating vts requires reboot

`mk install` lands a new vts binary at `/amd64/bin/vts`, but the
running vts daemon keeps the OLD binary in memory until process
exit. Killing the daemon kills all sessions, including the one
listen1 lives in. Best path: install the new binary, leave the
running daemon alone, reboot the VM when it's convenient. Pi9's
DECAWM-off escape is silently ignored by the old vts, so things
still work via the width-1 trick.

### Scroll offset doesn't auto-reset

This is intentional. Auto-reset on new content (the common
behavior in chat clients) is infuriating when you're trying to read
something. Pi9 leaves scrollOffset where you set it. ctrl+end
returns to live tail.

## Acceptance criteria

| Criterion | Verified |
|---|---|
| DECAWM in vts (code) | ✅ ~25 lines across 3 files, both builds clean |
| DECAWM in vts (runtime) | ⚠️ deferred until VM reboot (would kill listen1) |
| Pi9 emits ?7l / ?7h escapes | ✅ visible in source, harmless if unsupported |
| scrollOffset state + handlers | ✅ pgup/pgdn/shift+up/shift+down/ctrl+end |
| "scrolled N rows" status | ✅ shows when offset > 0 |
| `/$cputype/bin/pi9` install | ✅ `whatis pi9` returns `/bin/pi9` |
| `mkfile install` works | ✅ rule exists; uninstall + nuke + clean too |
| `pi9-install` one-shot | ✅ pulls binary, installs both files |
| Start menu Pi9 still works | ✅ launches new vtwin, pi9 auto-runs |
| Live OpenRouter call | ❌ skipped — no key provided |

## What's still deferred

After this phase:

- **Live LLM test** — needs key. Just `/login` and try a message.
- **Hot-restart vts** without killing sessions — would need vts to
  hand session state to a new process. Big change; defer.
- **Upstream the bubbletea plan9 shims** — still pending the PR.
- **Bug report `lrx-lrx` typo** in `x/term/term_plan9.go:95` — still
  pending.
- **Plumber rules** — "Send selection to pi9" workflow.
- **Pi.dev advanced commands** — `/scoped-models`, `/tree`, `/fork`,
  `/clone`, `/share`, `/changelog`.
- **`@file` references, `!shell` commands, image paste, multi-line
  input** — pi.dev features pi9 doesn't have.

## See Also

- [[pi9-phase8]] — what Phase 9 cleans up (width-1 trick origin)
- [[pi9-phase7]] — desktop integration that Phase 9 finishes
- [[vt-architecture]] — vts as a 9P file server
- [[vt100-parsing]] — what escapes vts now handles (DECAWM added)
- `src/vts/cells.h` `cells.c` `parser.c` — DECAWM patch
- `src/pi9/main.go` — scrollback nav + DECAWM negotiation
- `src/pi9/mkfile` — install rule
- `src/pi9/pi9-install` — one-shot installer
- `src/launcher/new-pi9` — prefers `/bin/pi9`
- `wiki/assets/pi9-phase9-rendered.png` — clean install screenshot
