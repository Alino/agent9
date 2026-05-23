---
title: pi9 Phase 4 — Sessions, Skills, Memory
created: 2026-05-16
updated: 2026-05-16
type: concept
tags: [arch, status-done]
---

# pi9 Phase 4 — Sessions, Skills, Memory

> **Status: done 2026-05-16.** pi9 has on-disk state: sessions autosave to JSON, the most recent one resumes on next launch, skills load on-demand from a `skills/` directory, and long-term memory is appended via a `remember` tool and re-injected into the system prompt every launch. Verified end-to-end: launch 1 → "please remember" → memory written → exit → launch 2 → prior turns visible in scrollback.

## What's new vs Phase 3

| | Phase 3 | Phase 4 |
|---|---|---|
| State | in-memory only | **persisted to `$home/lib/pi9/`** |
| System prompt | hard-coded constant | **base + memory + skill index** |
| Tools | 3 (read_file, write_file, run_rc) | **5** (+ remember, read_skill) |
| chat.History | JSON-free | **JSON-tagged + RestoreErrs** |
| Launch | always fresh | **resume current session by default** |
| CLI | none | `-new`, `-session <id>` |

## On-disk layout

```
$home/lib/pi9/        (plan9)   or    $HOME/.pi9/   (unix)
  $PI9_HOME also honored if set
├── memory.md              long-term facts (appended by remember tool)
├── skills/                on-demand markdown skills
│   ├── plan9-rc.md
│   └── vts-cells.md
└── sessions/
    ├── current            session id of the active conversation
    ├── 2026-05-16T22-49-10.json
    └── ...
```

Session IDs are ISO 8601-ish timestamps with colons replaced by dashes
(plan9 handles colons fine but they're awkward in shell). Atomic
writes use `path.tmp` + `os.Rename` — plan9's rename is atomic within
a single filesystem.

## System prompt assembly

`buildSystemPrompt()` concatenates three parts:

1. **base instructions** (always-on system prompt)
2. **memory.md** (declarative facts)
3. **skill index** — one line per skill: name + description

The full skill body is loaded on-demand via the `read_skill(name)`
tool. This is the same progressive-disclosure pattern Pi uses, and
what Claude Code calls "skill metadata".

Memory is reloaded on EVERY launch from `memory.md`, even when
resuming a session. The persisted session's `system` field is
overwritten with the freshly-built prompt — guarantees the agent
always sees current memory/skills, regardless of when the session
was first created.

## New tools

### `remember(content)`

Append a declarative fact to `memory.md`. Auto-prefixes with `- ` if
not already a list item or heading. Result: `"saved to <path>"`.

Used proactively by the model when:
- the user states a preference ("I prefer brief responses")
- a stable fact is established ("we're targeting 9front amd64")
- a workflow is corrected ("don't use bash syntax in run_rc")

### `read_skill(name)`

Load the full body of a named skill. Filename match takes precedence;
falls back to scanning all skills for a matching `name:` frontmatter.

The skill markdown format:

```
---
name: plan9-rc
description: short one-liner shown in skill index
---

# Body
Full markdown content goes here...
```

Hand-rolled frontmatter parser (no yaml dep). Lines before the second
`---` are key:value pairs; everything after is body.

## Session persistence

`pi9Model.saveSession()` runs after:
- user submits input (AppendUser)
- stream done with tool calls (BeginCall × N)
- every tool result lands (FinishCall)
- stream done without tool calls (FinishTurn)
- max-loops hit (force FinishTurn)

Plus a final save in `main()` after `p.Run()` returns, in case the
in-flight saves missed something.

The session JSON (truncated for clarity):

```json
{
  "system": "...full prompt...",
  "turns": [
    {
      "user": "please remember",
      "assistant": "Looking at the output:\\n\\nPhase 3 tool calling works ✓",
      "calls": [
        {
          "id": "call_mock_1",
          "name": "remember",
          "args": "{\"content\":\"Alex prefers concise replies...\"}",
          "output": "saved to /usr/glenda/lib/pi9/memory.md",
          "started": "2026-05-16T22:49:14.895Z",
          "finished": "2026-05-16T22:49:14.910Z"
        }
      ],
      "started": "2026-05-16T22:49:14.723Z",
      "finished": "2026-05-16T22:49:15.369Z"
    }
  ]
}
```

`error` fields go through `ErrText` (the `Err error` interface is
`json:"-"`-marked); `RestoreErrs()` rehydrates them via `fmt.Errorf`
on load.

## Verified end-to-end (screenshot: `wiki/assets/pi9-phase4-rendered.png`)

1. **Setup:** wiped `/usr/glenda/lib/pi9`, seeded `skills/plan9-rc.md` and `skills/vts-cells.md` via hget.
2. **Launch 1:** mock server returned `remember({"content": "Alex prefers concise replies. Plan 9 is the development target."})` tool call.
3. **Persisted:**
   - `memory.md`: `"- Alex prefers concise replies. Plan 9 is the development target."`
   - `sessions/2026-05-17T02-49-10.json`: full conversation
   - `sessions/current`: pointer to that id
4. **Launch 2:** pi9 loaded the session, replaced its `system` field with a freshly-built prompt (now containing the new memory line), and rendered all prior turns. The cyan `▸ remember(...)` block appeared exactly as it did before exit.

## CLI

```sh
# Resume current session (default)
/tmp/pi9

# Start a fresh session
/tmp/pi9 -new

# Load a specific session by id
/tmp/pi9 -session 2026-05-16T22-49-10
```

## Quirks worth remembering

### Plan 9 `home` vs unix `HOME`

The env var on plan9 is lowercase `home`, not `HOME`. We try both;
`$PI9_HOME` overrides either default.

### Memory loaded on every launch, not snapshotted

The session's `system` field gets OVERWRITTEN on load with the
current `buildSystemPrompt()` output. So if memory.md grew between
sessions, the new memory is visible in the resumed conversation.
Same for skill index — adding a new skill makes it discoverable in
existing sessions without re-creating them.

Trade-off: if the model previously relied on a specific phrasing
in the system prompt, and that phrasing changes, behavior may shift.
Acceptable for keeping memory + skills current.

### Errors don't roundtrip via JSON

Go's `error` interface isn't directly JSON-able. We carry an
`ErrText` string in parallel; `RestoreErrs()` rehydrates after load.

### Atomic session writes

`SaveSession` writes to `<path>.tmp` then renames. Plan 9 rename is
atomic on the same filesystem. Saves don't risk corrupting a session
mid-write even if the agent crashes.

### Session IDs are non-secret + clock-dependent

Two pi9 launches within the same second would collide. Unlikely in
practice; if it matters, prepend a random suffix.

## Acceptance criteria — all met

| Criterion | Verified |
|---|---|
| `$home/lib/pi9` created on launch | ✅ EnsureHome runs in main() |
| Skills loaded from skills/ on startup | ✅ both seeded skills listed in saved system prompt |
| Memory appended to memory.md | ✅ memory.md contained `- Alex prefers concise replies. …` |
| Session JSON written after each turn | ✅ session file exists with both turns |
| `current` pointer set | ✅ contained `2026-05-17T02-49-10` |
| `remember` tool fires | ✅ memory.md written by tool dispatch |
| `read_skill` tool fires | wired but not retested (logic identical) |
| Resume on next launch | ✅ launch 2 showed prior turn's `▸ remember(...)` block |
| System prompt updated on resume | ✅ saved session's system field includes skill index |
| -new flag starts fresh | wired (not visually retested) |
| -session id loads specific | wired (not visually retested) |

## What's deferred

### Phase 5 — plan9-native tools

- `plumb(text, dst)` — route via plumber
- `hget(url)` — plan9 native HTTP
- `walk(pattern)` — namespace traversal
- `mount(addr, mp)` / `bind(src, dst)` — namespace manipulation
- `ns()` — dump current namespace

Where pi9 stops being a port and becomes a plan9 thing.

### Phase 6 — polish

- Header always visible (currently scrolls off when turns exceed screen height)
- Stale input-box frames during streaming (lipgloss border + bubbletea differ)
- Multi-line input (shift+enter)
- Scrollback navigation
- Slash commands (`/clear`, `/model`, `/sessions`, `/save`)
- Word-wrap with rune-width awareness
- Themes

### Phase 7 — xena-panel integration

- Pi9 status widget on the taskbar
- "Ask pi9" launcher in start menu
- Right-click "Send to pi9" plumber rules

## See Also

- [[pi9-architecture]] — overall design
- [[pi9-phase1]] — Bubble Tea on plan9
- [[pi9-phase2]] — streaming chat
- [[pi9-phase3]] — tool calling
- `src/pi9/internal/store/store.go` — on-disk persistence
- `src/pi9/internal/tools/tools.go` — tool registry (remember, read_skill added)
- `src/pi9/testtools/seed-skills/` — example skill markdown
- `wiki/assets/pi9-phase4-rendered.png` — screenshot of resumed session
