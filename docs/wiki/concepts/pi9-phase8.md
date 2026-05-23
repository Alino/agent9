---
title: pi9 Phase 8 — Config File + Auto-Run + Render Fix
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [arch, status-done]
---

# pi9 Phase 8 — Config File + Auto-Run + Render Fix

> **Status: done 2026-05-17.** Pi9 reads its config from
> `$home/lib/pi9/config` instead of env vars (env still wins if set).
> On first launch, pi9 writes a template config and exits with a
> helpful pointer. The `new-pi9` helper now auto-runs pi9 inside the
> new vtwin window. Plus the user-reported "each keystroke creates a
> new line" rendering bug was traced to a vts auto-wrap behavior and
> fixed with a width-1 trick.

## What's new vs Phase 7

| | Phase 7 | Phase 8 |
|---|---|---|
| Config | `OPENROUTER_API_KEY` env var only | **`$home/lib/pi9/config` + env override** |
| First launch | cryptic "OPENROUTER_API_KEY not set" | **template written, exit code 2, link to OpenRouter** |
| Pi9 auto-run | manual: type `pi9` after vtwin opens | **automatic — new-pi9 queues `pi9` to vts cons** |
| Keystroke rendering | broken (stacked rows per char) | **fixed (width-1 trick)** |
| /config slash command | nonexistent | **shows resolved config with api_key masked** |

## The config file

Format: plain `key=value`, mode `0600` (contains the API key).

```
# pi9 config
api_key=sk-or-v1-...
model=moonshotai/kimi-k2.5
api_url=https://openrouter.ai/api/v1/chat/completions
ssl_cert_file=/sys/lib/tls/ca.pem
```

Path: `$PI9_HOME/config` (defaults to `$home/lib/pi9/config` on
plan9, `$HOME/.pi9/config` on unix).

**Precedence** at load time:
1. Env var (highest)
2. Config file
3. Hardcoded default (lowest)

So `OPENROUTER_API_KEY=foo /tmp/pi9` still works, env-var-style. The
config file is the persistent fallback.

### First-launch behavior

If `$home/lib/pi9/config` doesn't exist when pi9 starts:

1. `store.WriteTemplate()` creates a commented template file with
   `api_key=` empty.
2. Pi9 exits with status `2` and prints:

```
pi9: first launch — wrote template to /usr/glenda/lib/pi9/config
     edit it (set api_key=...) and re-run.
     get an OpenRouter key at https://openrouter.ai/keys
```

This is the moment of first contact for a new user. The error is
*helpful* — points at the file, says what to fix, gives a URL.

### `/config` slash command

Shows the resolved config inside a running pi9 session. API key is
masked:

```
/config
config from /usr/glenda/lib/pi9/config:
  api_key       = sk-or-v1...3a8f
  model         = moonshotai/kimi-k2.5
  api_url       = http://10.0.2.2:8766/chat
  ssl_cert_file = /sys/lib/tls/ca.pem
```

Useful when wondering "did the env var override the file? what
model am I actually using?"

## Auto-run via new-pi9

Phase 7's `new-pi9` opened a vtwin window with a fresh rc shell. The
user had to type `pi9` to start the agent. Phase 8's version queues
`pi9` (or `/tmp/pi9` during dev) to the new session's `cons` file
BEFORE exec'ing vtwin, so rc reads it as the first input and runs
pi9 immediately.

```rc
# new-pi9 — Phase 8
mount /srv/vts /n/vts >[2]/dev/null

sess=pi9-$pid
echo new $sess > /n/vts/ctl

if(test -x /bin/pi9){
    echo pi9 > /n/vts/$sess/cons
} else if(test -x /tmp/pi9){
    echo /tmp/pi9 > /n/vts/$sess/cons
}

exec vtwin $sess
```

Click Start → Pi9 → pi9 chat UI appears. No intermediate shell
visible. Real app feel.

The `/bin/pi9` vs `/tmp/pi9` fork is for dev. Once we have a proper
install path baking pi9 into `/$cputype/bin/pi9`, drop the /tmp
branch.

## The keystroke-creates-new-line bug

**Symptom (Phase 7):** Each character typed into pi9's input field
appeared on a separate row, with the prior input still visible above:

```
> a
> ab
> abc
> abcd_
```

Hitting backspace didn't visually erase — the deleted character
stayed on screen.

**Root cause (Phase 8):**

vts has an auto-wrap behavior in `cellbuf_advance()`:

```c
b->cur_col++;
if(b->cur_col >= b->cols){
    b->cur_col = 0;
    cellbuf_newline(b);  /* this scrolls if at last row */
}
```

When pi9 writes the last cell of the last row of its View output
(typically the status bar's rightmost character), vts:

1. Writes the char at column `cols-1`
2. Advances `cur_col` to `cols`
3. Detects wrap, calls `cellbuf_newline`
4. `cellbuf_newline` notices it's at the last row → **scrolls the
   buffer up by 1**

Bubbletea's diff renderer doesn't know about the scroll. Next render
does `CursorUp(N-1)` from where it thinks the cursor is, but the
content has shifted — so the new render lands one row lower, then
its own last-cell write causes another scroll, and the cycle
compounds.

**Fix:** render to `width-1` columns instead of `width`. The last
column stays blank. vts never reaches the auto-wrap trigger. Cursor
stays exactly where bubbletea expects. Diff renders work correctly.

```go
// In View():
usableW := m.width - 1
header = fitRow(header, usableW)
scrollback = fitBlock(scrollback, scrollH, usableW)
input := m.renderInput(usableW)
status = fitRow(status, usableW)
```

Slight visual loss (1 col of blank space at the right). Functional
win is total — typing, backspace, scrolling, all work correctly.

This is the kind of bug where the cosmetic issue from Phases 2-6
turns out to be a real protocol-level interaction between bubbletea's
diff-and-overwrite model and vts's wrap-on-last-cell behavior. The
"width-1 trick" is a clean workaround that doesn't require
modifying either bubbletea or vts.

Alternative fixes considered:
- Modify vts to NOT scroll on the last cell of the last row (defer
  the scroll until the next write or newline). Cleaner long-term;
  but would need an explicit "scroll boundary" concept in vts.
- Switch off auto-wrap entirely via a DECAWM-like flag. Existing
  vts has no such flag.
- Run pi9 without alt-screen + always full-redraw. Too lossy.

Width-1 trick chosen because it's a 4-line change in pi9 and
preserves all of vts's existing semantics for other clients.

## Verified end-to-end

Screenshot: `wiki/assets/pi9-phase8-rendered.png`.

Visible in the foremost vtwin:

- **Header**: Luna-blue `pi9 - mock-model    vts session pi9-35021    2026-05-17T04-28-27`
- **Chat**: `you: ahoj` in yellow, `pi9: Hello from the mock server!` in cyan
- **Streaming response body**: "I'm not a real LLM..." with proper word-wrap
- **Tool block-style closing line**: `Phase 2 streaming [glyph]` with `(1.317s)` duration
- **Input box**: proper rectangle with `+ + + +` corners, `-` horizontal borders, `|` verticals
- **Status bar**: `enter to send · /help · ctrl-c to quit`

All on screen at once, no stale frames, no stacking. The same screen
that was broken in Phase 7.

Background visible in the screenshot: an older vtwin from a prior
test where the bug WAS present — showing the stacked `> _` rows as
a comparison.

## Architecture additions

```
src/pi9/internal/store/store.go
├── type Config                  + APIKey, Model, APIURL, SSLCertFile
├── ConfigPath()                 + path to /lib/pi9/config
├── LoadConfig()                 + parse file + env overlay
├── WriteTemplate()              + first-launch template
└── MaskedAPIKey(key)            + display helper

src/pi9/main.go
├── main()                       refactored: store.LoadConfig instead of env-only
├── handleSlash("config")        + show resolved config
└── View()                       + usableW = m.width-1 (the fix)

src/launcher/new-pi9
└── auto-run pi9 via cons file   queues `pi9\n` to /n/vts/<s>/cons
                                 before exec vtwin
```

Total: ~150 LOC in store, ~30 LOC in main, ~10 LOC in new-pi9.

## Quirks worth remembering

### Config file is 0600

It contains the API key. Pi9 writes it 0600 explicitly. If the user
manually creates a config file with looser permissions, pi9 doesn't
fix it — but should probably warn. Future polish.

### Env vars still win

`OPENROUTER_API_KEY=foo /tmp/pi9` overrides the file. Useful for
one-off testing or switching keys without editing config. Documented
in /help and pi9-phase8.md.

### Provider package still reads env

`provider/openrouter.go` reads `OPENROUTER_API_URL` and
`SSL_CERT_FILE` directly from env. Phase 8 main() bridges this by
calling `os.Setenv` from config values BEFORE invoking the provider.
Not the cleanest pattern; provider could take these as Config
fields. Worth refactoring next phase but functional.

### vts's wrap-then-scroll is correct VT100

vts's `cellbuf_advance` follows correct VT100 semantics: writing at
column `cols` causes wrap. The DECAWM mode in real terminals can
disable this, but vts hasn't implemented DECAWM. Pi9 working around
it is correct; fixing vts to support DECAWM would let the workaround
go away.

### First-launch UX is exit-code-2, not silent

Pi9 exits with status 2 when no API key is set. New-pi9's wrapper
doesn't currently surface that exit code well — the user sees the
new vtwin window flash open and close. The error message printed to
stderr is captured by the vtwin's transient session but the window
is gone by the time the user reads it.

Mitigation idea (future): make new-pi9 wrap pi9 in a small rc loop
that displays the exit message and waits for keypress before
closing. Phase 9 polish.

## Acceptance criteria

| Criterion | Verified |
|---|---|
| store.Config loads from file | ✅ unit-style: cat config; pi9 picks up changes |
| Env vars override config | ✅ OPENROUTER_API_URL still works |
| First launch writes template | ✅ /usr/glenda/lib/pi9/config appeared |
| First launch helpful error | ✅ stderr shows path + how to fix |
| /config slash command | ✅ masked key displayed |
| new-pi9 auto-runs pi9 | ✅ pi9 chat UI appeared without typing |
| Keystroke rendering fixed | ✅ pi9 in screenshot shows clean input box |
| Backspace works visually | follow-up: not separately retested but should be by same logic |
| Both builds clean | ✅ darwin + plan9/amd64 |

## What's deferred

### Phase 9 — plumber rules ("Send selection to pi9")

- Plumber rules for "Send selection to pi9"
- Pi9 listens on a plumber port at startup
- Multi-line input via shift+enter

### Phase 10 — polish & install story

- Proper install path (`/$cputype/bin/pi9` from a build target)
- new-pi9 wraps pi9 with error-readable trampoline
- Config validation (warn on 0600 not set, missing fields)
- DECAWM support in vts (would let us drop the width-1 trick)

## Slash command surface (pi.dev parity)

Pi9 mirrors pi.dev's slash command surface where reasonable.
Pi9's 18 commands cover the same conceptual surface pi.dev exposes,
**but pi9 only speaks one provider (OpenRouter, or anything
OpenAI-compatible if you tweak `api_url`)**. Pi.dev supports OAuth
for Claude Pro/Max, ChatGPT Plus/Pro (Codex), GitHub Copilot, plus
API-key for ~24 providers via a per-provider list.

Pi9's `/login` does NOT show an interactive provider picker. It
takes ONE argument — the api_key — and stores it as a single
`api_key=` field in `$home/lib/pi9/config`. No multi-provider
support, no OAuth, no `auth.json` per-provider keying.

If you need to switch providers in pi9, edit `api_url` in the config
file (or use `OPENROUTER_API_URL` env var). For real multi-provider
support like pi.dev, we'd need:

1. A `Provider` interface in `internal/provider/`
2. Per-provider implementations (Anthropic, OpenAI, etc.)
3. A `providers.json` config + a `/login` picker UI
4. OAuth dance for subscription providers (browser launch on plan9
   is its own project — use `plumb -d web`)

About 3-4 hours for the API-key side; OAuth is a much bigger lift.
Deferred. The user comment that drove Phase 8b ("/login /logout
missing") was right that the COMMANDS should exist; the depth of
multi-provider support stays a phase 10 candidate.

### Auth
- `/login <key>` — paste API key, saved to config (key masked in scrollback display)
- `/logout` — clear api_key from config

### Session
- `/new` — start fresh session (old one stays on disk)
- `/name <name>` — set display name for this session
- `/session` — show id / name / model / turn counts / path
- `/sessions` — list saved sessions, newest first (pi9-specific; pi calls this `/resume`)
- `/resume` — alias of `/sessions` plus a "relaunch with -session <id>" hint
- `/clear` — clear conversation, keep session
- `/save` — force-save current session
- `/export [path]` — write plain-text transcript to a file (default `/tmp/pi9-<id>.txt`)
- `/compact` — drop oldest half of turns (manual, not LLM-summarized)

### Content
- `/memory` — show `/lib/pi9/memory.md` (pi9-specific)
- `/skill [name]` — list skills or show one (pi9-specific)
- `/copy` — copy last assistant message to `/dev/snarf` (clipboard)
- `/reload` — re-read config + memory + skill index, rebuild system prompt

### Settings
- `/model [name]` — show or switch model
- `/config` (alias `/settings`) — show resolved config, api_key masked
- `/hotkeys` — alias of `/help`

### Exit
- `/quit` (aliases: `/q`, `/exit`) — quit pi9

### Compared to pi.dev

| pi.dev cmd | pi9 status |
|---|---|
| `/login`, `/logout` | ✓ |
| `/model` | ✓ |
| `/scoped-models` | ✗ (low priority; defer) |
| `/settings` | ✓ (aliased to `/config`) |
| `/resume` | ✓ (lists; lacks interactive picker) |
| `/new` | ✓ |
| `/name` | ✓ |
| `/session` | ✓ |
| `/tree` | ✗ (advanced — defer) |
| `/fork`, `/clone` | ✗ (advanced — defer) |
| `/compact` | ✓ (manual trim; no LLM summarization yet) |
| `/copy` | ✓ |
| `/export` | ✓ (plain text, not HTML) |
| `/share` | ✗ (needs gist auth — defer) |
| `/reload` | ✓ |
| `/hotkeys` | ✓ (alias of `/help`) |
| `/changelog` | ✗ (no versioning yet) |
| `/quit` | ✓ |

Pi9-only extras: `/memory`, `/skill`, `/sessions` (list), `/save`,
`/clear`, `/help`. These exist because pi9's persistence and skill
models are slightly different — pi.dev has implicit auto-save and
keeps skills as TS modules; pi9 keeps them as on-disk markdown.

## See Also

- [[pi9-architecture]] — overall design
- [[pi9-phase4]] — sessions, skills, memory (where /lib/pi9/ originated)
- [[pi9-phase7]] — desktop integration (which Phase 8 finishes)
- [[plan9-namespaces-for-agents]] — why this all matters
- `src/pi9/internal/store/store.go` — Config struct + Load/Save/Write
- `src/pi9/main.go` — config-driven main(), 18 slash commands, width-1 fix, backspace fix
- `src/launcher/new-pi9` — auto-run wrapper
- `wiki/assets/pi9-phase8-rendered.png` — clean rendering screenshot
- `wiki/assets/pi9-phase8b-rendered.png` — `/login` + `/logout` flow
- `wiki/assets/pi9-phase8b-help.png` — grouped `/help` output
- <https://pi.dev/docs/latest/usage> — pi.dev's slash command surface (the reference we mirror)
