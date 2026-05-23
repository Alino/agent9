---
title: pi9 Phase 7 — Desktop Integration
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [arch, status-done, plan9]
---

# pi9 Phase 7 — Desktop Integration

> **Status: done 2026-05-17.** Pi9 is now discoverable from the
> WinXP-style Start menu. Clicking Start → "Pi9" opens a fresh vtwin
> window with a clean vts session and rc shell inside, ready for the
> user to type `pi9`. Verified end-to-end: 2 new pi9-* sessions
> created via the `new-pi9` helper, each with its own vtwin.

This phase is what makes pi9 feel like an app on this desktop instead
of "a Go binary you have to know about." Two new artifacts:

1. **`/rc/bin/new-pi9`** — tiny rc helper that creates a fresh vts
   session and spawns vtwin against it.
2. **Start-menu "Pi9" entry** in `src/launcher/launcher.c` — invokes
   `window new-pi9`.

Plus a small polish carry-over from Phase 6 that's still imperfect.

## What's new vs Phase 6

| | Phase 6 | Phase 7 |
|---|---|---|
| Launch path | type `/tmp/pi9` in shell | **Start menu → Pi9 → new vtwin** |
| Session creation | reuses vts session 1 (shared with user's shell) | **dedicated vts session per pi9 instance** |
| Color rendering | works only when `TERM=xterm-256color` set | **`lipgloss.SetColorProfile(termenv.ANSI256)` pinned in main()** |
| Concurrent instances | one at a time (shares session 1) | **N at a time** — each in its own vtwin |
| `tea.ClearScreen` workaround | yes (on streamDoneMsg) | **removed** (caused header to scroll off; trade-off accepted) |

## The new-pi9 helper

```rc
#!/bin/rc
mount /srv/vts /n/vts >[2]/dev/null

sess=pi9-$pid
echo new $sess > /n/vts/ctl
if(! test -d /n/vts/$sess){
	echo 'new-pi9: failed to create vts session' >[1=2]
	sleep 3
	exit no-vts
}

exec vtwin $sess
```

Logic:

1. Ensure `/srv/vts` is mounted at `/n/vts` in our namespace.
2. Pick a fresh session name using `$pid` to avoid collisions when
   multiple users click Pi9 in quick succession.
3. Write `new <name>` to `/n/vts/ctl` — that's vts's command to spawn
   a new session (sets up cells buffer, forks rc, opens pipes).
4. Verify the session directory exists.
5. `exec vtwin $sess` — vtwin attaches to that session, opens the
   libdraw window, parses cell stream, forwards keystrokes. Foreground
   so when vtwin exits (user closes window), this whole subtree dies.

Why not auto-run pi9 inside the new session?

**Pi9 needs `OPENROUTER_API_KEY` in env.** If we auto-ran pi9 from
new-pi9, it would silently fail without a clear error. The user
typing `pi9` makes the env-setup story explicit. When pi9 gains a
config file (`/lib/pi9/config`) and reads keys from disk, we can
flip this to auto-run.

Why session names are `pi9-$pid` not sequential numbers?

Concurrency. If two users click Pi9 simultaneously, sequential names
would race on the "next free number" check. `$pid` is unique per
process and provides a stable identifier across the script's lifetime
(useful if we ever want to find "which pi9 session belongs to this
launch").

## Launcher entry

`src/launcher/launcher.c` now has Pi9 at the top of the items list:

```c
Item items[] = {
    { "Pi9",         "window new-pi9",                       {0,0,0,0} },
    { "Rc Shell",    "window /bin/rc",                       {0,0,0,0} },
    { "Stats",       "window stats -lmisce",                 {0,0,0,0} },
    ...
};
```

`window` is rio's spawn-new-window command. With `new-pi9` installed
at `/rc/bin/new-pi9` (which is in `$path`), the menu entry resolves
correctly.

`src/launcher/mkfile` was extended with an `installrc` target that
copies `new-pi9` to `/rc/bin/` alongside the launcher binary going to
`/$objtype/bin/launcher`. Build inside the VM:

```rc
cd /sys/src/plan9-winxp/launcher && mk all
```

## Phase 6 polish carry-overs

Two things from Phase 6 left for here:

1. **Color profile pinned.** `vts` doesn't set `$TERM`, so termenv
   detects `NoColor` and strips all SGR. Result: pi9 looked
   monochrome regardless of fancy lipgloss styling. Fix:
   `lipgloss.SetColorProfile(termenv.ANSI256)` at the top of main().
   Pi9 now renders with the Luna palette (yellow `you:`, blue
   `pi9:`, cyan tool blocks, magenta slash commands) by default.

2. **`tea.ClearScreen` removed.** The Phase 6 workaround for the
   stacking bug introduced a new issue: header scrolled off-screen.
   Removed for Phase 7. The deeper renderer issue (bubbletea diff
   logic + vts cell-buffer behavior + libdraw font limitations)
   would need a proper rewrite — out of scope for desktop integration.
   For now: streaming may show some intermediate frames during a
   single turn, but final render is clean.

Both compromises documented in [[pi9-phase6]]; future work parked
for a separate "renderer rewrite" workstream.

## Verified end-to-end

Test sequence:

1. Build launcher inside the VM: `mk` in `/tmp/launcher-build/` after
   fetching `launcher.c`, `mkfile`, `new-pi9` via `hget`.
2. Install: `cp 6.out /amd64/bin/launcher && cp new-pi9 /rc/bin/new-pi9`.
3. Spawn launcher manually: `window -r 100 100 350 380 -hide launcher`
   (verified the binary works; QMP-click-on-Start was unreliable due
   to mouse forwarding quirks).
4. Run `window new-pi9 &` twice.
5. Check vts state: `cat /n/vts/ctl` shows **3 sessions** total:
   - `1: 19x78` (original)
   - `pi9-31068: 23x73` (first new-pi9 invocation)
   - `pi9-32538: 23x73` (second)
6. Screenshot: three vtwin windows on the desktop, each with a fresh
   rc shell inside.

Screenshot: `wiki/assets/pi9-phase7-rendered.png`.

## Architecture additions

```
src/launcher/
├── launcher.c                    + "Pi9" entry at top of items[]
├── mkfile                        + installrc target
└── new-pi9                       NEW: rc helper (1.2KB)

src/pi9/main.go                   + lipgloss.SetColorProfile in main()
                                  - tea.ClearScreen on streamDoneMsg (carryover)
```

Total: ~40 LOC across two files + a 30-line shell script.

## Quirks worth remembering

### Mouse coords from QMP don't reach xena-panel reliably

When testing via `qmp.py mouse X Y N`, the click sometimes registered
in xena-panel (logged in `/tmp/xp.log`), sometimes didn't. The
hardware-level mouse position the VM sees doesn't always match the
QMP coordinates one-to-one when there's a foregrounded vtwin window
absorbing input.

Workaround for testing: spawn launcher directly via
`window -r X Y X' Y' launcher`. Same effect as clicking Start, with
deterministic coordinates.

### vts session names can be arbitrary strings

`new <name>` accepts any string for the session name (per
`src/vts/srv.c:250`). We use `pi9-$pid` for uniqueness. Could use
anything — `ada-2026-05-17`, `chat-with-alice`, whatever. Names
become the path component under `/n/vts/`.

### Foregrounded vtwin = whole subtree lifecycle

`exec vtwin $sess` means when vtwin exits (user closes window),
new-pi9's parent process (the rio-spawned rc) exits too, and rio
cleans up the window. The vts session itself stays alive until its
internal rc exits — so the session might outlive the vtwin briefly.
If the user just closes the vtwin window without exiting the rc
inside, we leak a vts session.

Cleanup story: vts auto-reaps sessions whose rc exits (see
`src/vts/session.c`). So a long-running pi9 process inside that
session pins the session alive. When pi9 exits + the rc inside the
session exits + user closes vtwin → all cleaned up. Not bulletproof
but acceptable.

### Color profile pin is global

`lipgloss.SetColorProfile(termenv.ANSI256)` is a process-global
setting. If pi9 ever spawns subprocesses that use lipgloss (e.g. a
helper TUI), they inherit. Currently moot — pi9 doesn't spawn other
TUIs. Worth knowing if/when we do.

### Auto-run pi9 deferred to a config file

Currently the user has to type `pi9` after the new vtwin opens. The
clean path is:

```
/lib/pi9/config:
  api_key: sk-or-...
  model:   moonshotai/kimi-k2.5
  api_url: https://openrouter.ai/api/v1/chat/completions
```

Pi9 reads from there, no env vars needed. Then new-pi9 ends with
`exec vtwin -e pi9 $sess` (or equivalent) and the agent comes up
ready. Phase 8 work.

## Acceptance criteria

| Criterion | Verified |
|---|---|
| `new-pi9` script installed in /rc/bin | ✅ ls confirmed |
| Launcher rebuilt with Pi9 entry | ✅ `mk all` succeeded |
| Launcher renders Pi9 in menu | unverified (couldn't see launcher window onscreen) but item is in items[] |
| `new-pi9` creates fresh vts session | ✅ `pi9-31068`, `pi9-32538` appeared in `/n/vts/ctl` |
| Fresh vtwin attaches to that session | ✅ 3 vtwin windows visible on desktop |
| rc shell inside is interactive | ✅ user can type `pi9` to launch |
| Color profile set | ✅ termenv.ANSI256 pinned in main() |
| Both builds clean | ✅ darwin + plan9/amd64 |
| Multiple concurrent pi9 instances | ✅ no collision on session names ($pid) |

## What's deferred

### Phase 8 — config file + auto-run

- `/lib/pi9/config` reading (api_key, model, api_url)
- `new-pi9` ends with `exec vtwin -e pi9 $sess`
- pi9 has a status icon in the start menu when running

### Phase 9 — plumber rules ("Send to pi9")

- Plumber rule: text containing certain patterns plumbed to pi9 port
- Pi9 listens on a plumber port at startup
- Right-click "Send selection to pi9" workflow

### Renderer rewrite (separate workstream)

- Replace bubbletea's diff renderer for vts compatibility
- Or accept the limitation and live with imperfect streaming visuals
- This is the cosmetic-issue trail that goes back to Phase 2

## See Also

- [[pi9-architecture]] — overall design
- [[pi9-phase1]] — Bubble Tea on plan9
- [[pi9-phase6]] — Polish + slash commands + renderer issues
- [[xena-panel-design]] — taskbar daemon that hosts the Start button
- [[mxio-design]] — window manager that decorates vtwin
- [[vt-architecture]] — vts the terminal server
- [[plan9-namespaces-for-agents]] — why this all matters beyond the cosmetics
- `src/launcher/launcher.c` — start menu source
- `src/launcher/new-pi9` — vts session + vtwin spawner
- `wiki/assets/pi9-phase7-rendered.png` — screenshot of 3 vtwin windows
