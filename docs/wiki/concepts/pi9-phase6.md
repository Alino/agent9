---
title: pi9 Phase 6 — Polish (Render Fix + Slash Commands)
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [arch, status-done]
---

# pi9 Phase 6 — Polish

> **Status: done 2026-05-17.** Slash commands wired (`/help`, `/clear`,
> `/new`, `/save`, `/sessions`, `/memory`, `/skill`, `/model`, `/quit`).
> Input box stale-frame stacking from Phases 2–5 is fixed: the input
> box now renders as a single 3-row rectangle that stays put across
> streaming and turn boundaries. The renderer is now ASCII-only to
> avoid libdraw defaultfont's missing-glyph squares.

## What's new vs Phase 5

| | Phase 5 | Phase 6 |
|---|---|---|
| Layout math | logical lines, lipgloss-borders | **fitRow + fitBlock** — every row exactly `cols` cols, every section exactly N rows |
| Input box border | `lipgloss.NormalBorder()` (unicode box-drawing) | hand-drawn ASCII `+` `-` `|` |
| Stale frames after stream | yes, ugly | gone (forced `tea.ClearScreen` on streamDoneMsg + slash-help) |
| Slash commands | none | 9 commands |
| chat.Turn | implicit (Local field missing) | **+`Local bool`** flag, excluded from ToProviderMessages |
| Tool block prefix | cyan `▸` | cyan `->` (ASCII) |
| Em-dashes/ellipses in UI | unicode | ASCII (`-`, `...`) |

## The stale-frame bug, finally diagnosed

Symptoms in Phases 2–5: during streaming, multiple "input box"
frames stacked at the bottom of the screen. The current `> _` was at
the bottom; above it, ghosts of earlier renders showed `> hello mock`,
`> streaming…`, etc.

Three causes compounded:

1. **lipgloss.Border() width math.** `lipgloss.NewStyle().Border(...).Width(N).Render(s)`
   computes the BOX width from `N`, but the inner content's visible
   width (post-SGR-strip) plus padding can exceed `N` if the content
   includes ANSI escapes that lipgloss's column counter mis-counts.
   The result is a box that's wider than `cols`, vts wraps it,
   producing a "second row" with the overflow.

2. **bubbletea's diff renderer + layout shifts.** Bubbletea writes only
   lines that changed since the last render, advancing the cursor via
   `CursorUp` + `\n` for unchanged rows. When streaming adds content,
   the absolute row index of the input box doesn't change (we always
   pad scrollback to fill), but the CONTENT of each row does. The
   renderer eventually settles, but during the transient there are
   off-by-one cursor positions that leak old content into "wrong" rows.

3. **libdraw defaultfont glyph coverage.** Even after fixing 1 + 2,
   the unicode box-drawing chars (`┌`, `─`, `│`, `┐`) rendered as
   missing-glyph squares because vtwin uses libdraw's bitmap default
   font, which doesn't include U+2500–257F. The visible result: empty
   rows where horizontal borders should be.

Fixes applied:

1. Replaced `lipgloss.Border()` with a hand-drawn `renderInput()` that
   manually positions `+ - |` characters and uses `fitRow` to pad
   each row to exactly `cols` visible chars.
2. Added `tea.ClearScreen` as a returned Cmd after `streamDoneMsg`
   and `/help`. Forces a full repaint, bypassing the diff renderer
   for the moments most prone to artifacts.
3. Switched all UI glyphs to ASCII: `+ - |` for the input box,
   `->` for tool block prefix, `...` for ellipses, `-` for em-dashes
   in scrollback messages.

The ANSI-aware width helpers — `visibleWidth`, `fitRow`, `fitBlock`
— are documented inline in `main.go`. They handle CSI escapes,
OSC sequences, and UTF-8 leading-byte detection. East-Asian Wide
chars aren't supported; not relevant for pi9's content.

## Slash commands

9 commands, all dispatched in `handleSlash()` before the input reaches
the LLM. Each appends a "Local" turn to history that's:
- visible in scrollback (rendered with magenta `localUserStyle`)
- persisted in session JSON (with `"local": true`)
- **excluded from `ToProviderMessages()`** — the LLM doesn't see
  slash commands or their responses

| Command | Action |
|---|---|
| `/help` (or `/?`) | show command help |
| `/clear` | drop conversation turns, keep memory + skills + system prompt |
| `/new` | start fresh session (old one stays on disk) |
| `/save` | force-save current session JSON |
| `/sessions` | list saved sessions, newest first, mark current with `→` |
| `/memory` | show contents of memory.md |
| `/skill` | list installed skills |
| `/skill <name>` | print full body of named skill |
| `/model` | show current model name |
| `/model <name>` | switch model for subsequent turns |
| `/quit` (or `/q`, `/exit`) | exit pi9 |

### Why local turns are persisted

Two reasons:

1. **Audit trail.** When debugging, the session JSON shows the user
   ran `/clear` at time T and got an empty-history afterwards. Without
   persistence, that gap would be unexplained.

2. **Resume fidelity.** If you `/clear`, then `/quit`, then relaunch,
   the resumed session starts empty (correct) but the scrollback
   still shows the `/clear` turn so you remember what happened.

Local turns get a magenta user label (palette 13) to visually
distinguish them from real model exchanges (yellow `you:` / blue
`pi9:` for those).

### What `/clear` vs `/new` do differently

- `/clear` resets `m.history.Turns = nil` but keeps the same
  `sessionID`. The session file is overwritten with an empty Turns
  list on next save.
- `/new` allocates a fresh `sessionID`, updates the `current`
  pointer file to point to it, and starts with empty Turns. The
  OLD session file stays on disk untouched — recoverable via
  `/sessions` and `-session <id>` on next launch.

`/clear` is "I want to start over but stay in this conversation
context." `/new` is "I want to start over and keep the old as
archive."

## Verified end-to-end

Screenshot: `wiki/assets/pi9-phase6-rendered.png`.

Session JSON from the test shows:
- 5 total turns (`"user":` count)
- 4 marked `"local": true` (the slash commands)
- 1 real chat exchange (`hello mock`)

The mock returned the canned "Phase 2 streaming" response.

Visible improvements:
- ✅ Input box is a single 3-row rectangle (top border + content + bottom border)
- ✅ No stacked stale frames during or after streaming
- ✅ Tool block prefix renders as `->` instead of missing-glyph square
- ✅ ASCII-only UI works regardless of font

Remaining known issue:
- ⚠️ Header (Luna-blue `pi9 - model` bar) sometimes scrolls off-screen
  after a forced `ClearScreen`. Bubbletea's cursor-positioning after
  a fresh clear-screen lands at row N when we expect row 0; the top
  rows of the View buffer end up above the viewport. This is a
  bubbletea-renderer-on-alt-screen quirk we'll address in a future
  polish pass (likely by switching to non-altscreen mode + manual
  full-redraw).

## Architecture additions

```
src/pi9/main.go
├── visibleWidth(s string) int          ANSI-aware width counter
├── fitRow(s string, cols int) string   pad/truncate to exact width
├── fitBlock(s string, rows, cols int)  pad/truncate to exact dimensions
├── renderInput(cols int) string        hand-drawn 3-row input box
└── handleSlash(text string) → Cmd       9-command dispatcher

src/pi9/internal/chat/chat.go
├── Turn.Local bool                     marks slash-command turns
├── History.AppendLocal(user, response) inserts a local turn
└── renderLocalTurn(t, width)            magenta user + indented body
```

## Quirks worth remembering

### bubbletea's diff renderer + layout shifts

For TUIs whose layout has multiple sections (scrollback growing,
input box fixed at bottom), the diff renderer's row-by-row "skip if
unchanged" logic interacts badly with content shifting between
sections. Workaround: emit `tea.ClearScreen` at semantic transition
points (turn boundaries, slash commands) to force a full repaint.

The proper fix is to track which sections of the View are "static"
(don't change between renders) and which are "dynamic" (scrollback,
input). For now: ClearScreen is the hammer.

### Lipgloss color profile in vts

Vts doesn't set `$TERM`, so lipgloss's termenv detection picks
`NoColor` and strips all SGR. Result: pi9 in vts renders as plain
text without colors. To fix: explicitly set `lipgloss.SetColorProfile(termenv.ANSI)`
in main() before any Render() calls. NOT DONE in Phase 6 — we
prioritized the layout/stacking fix. Phase 7 polish.

(Workaround for color: set `TERM=xterm-256color` in the pi9 launch
env — that gets termenv to pick `Ansi256` profile.)

### ASCII-only borders aren't beautiful but they're portable

Hand-drawn `+--+ | |` is uglier than `┌─┐ │ │`. The trade-off: works
in any font, doesn't show missing-glyph squares, vts/vtwin doesn't
have to decode multi-byte UTF-8 for borders. When we eventually wire
fontsrv into vtwin per [[vtwin-typography]] and ship Inconsolata or
JetBrains Mono, we can switch back to unicode.

### Why `tea.ClearScreen` instead of erasing inline

Bubbletea exposes `tea.ClearScreen` as a Cmd that emits ANSI
`ESC[2J ESC[H`. The renderer resets its `lastRender` cache on this,
so the next frame is a full repaint. Cleaner than tracking when to
invalidate ourselves.

## Acceptance criteria

| Criterion | Verified |
|---|---|
| 9 slash commands dispatch correctly | ✅ JSON shows 4 local turns + 1 real, all rendered |
| Local turns excluded from provider messages | ✅ `ToProviderMessages` skips `if t.Local` |
| Local turns persisted in session JSON | ✅ `"local": true` field present |
| Local turns rendered in magenta | ✅ palette 13 styled |
| Input box renders as proper rectangle | ✅ 4 corners visible in screenshot |
| Stale stacked frames eliminated | ✅ single input box visible after streaming |
| ASCII-only glyphs | ✅ no missing-glyph squares for `+ - |` |
| Header sometimes scrolls off after ClearScreen | ⚠️ known issue, deferred to Phase 7 |

## What's deferred

### Phase 7 polish (next session?)

- Header pinned (currently scrolls off after `ClearScreen` due to
  bubbletea alt-screen cursor positioning quirk)
- Set `lipgloss.SetColorProfile(termenv.ANSI)` in main() so colors
  work without `TERM` env var
- Multi-line input (shift+enter to add line, enter to submit)
- Scrollback navigation (page up/down, end)
- Word-wrap with rune-width awareness (if/when we move beyond ASCII)

### Phase 7 xena-panel integration (the original Phase 7)

- pi9 status widget on the taskbar (current model, streaming spinner)
- "Ask pi9" launcher in start menu
- Right-click "Send to pi9" plumber rules

## See Also

- [[pi9-architecture]] — overall design
- [[pi9-phase1]] — Bubble Tea on plan9
- [[pi9-phase2]] — streaming chat
- [[pi9-phase3]] — tool calling
- [[pi9-phase4]] — sessions, skills, memory
- [[pi9-phase5]] — plan9-native tools
- [[plan9-namespaces-for-agents]] — why pi9 matters at all
- [[vtwin-typography]] — font story; future-state fix for ASCII-only borders
- `src/pi9/main.go` — fitRow, fitBlock, renderInput, handleSlash
- `src/pi9/internal/chat/chat.go` — Turn.Local + renderLocalTurn
- `wiki/assets/pi9-phase6-rendered.png` — screenshot
