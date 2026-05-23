---
title: pi9 — Plan 9-native LLM Agent
created: 2026-05-16
updated: 2026-05-16
type: concept
tags: [arch, status-wip]
---

# pi9 — Plan 9-native LLM Agent

A pi-shaped LLM coding agent that runs **inside vts** as just another Go
process, looks like a modern terminal agent, and integrates with plan9's
namespace primitives. Heir to `tinyxena` (see [[../../../plan9-agent/README.md|tinyxena]]),
target runtime [[vt-architecture|vts]] / [[vt-9p-namespace|vtwin]].

## Why pi9 (vs alternatives)

- **Not Hermes-on-plan9.** Hermes is the Mac hub (Telegram, WhatsApp,
  voice, image gen). Different scope. pi9 = your plan9 hacking companion.
- **Not Pi-rewrite.** Pi is TypeScript + Node. Node won't run on plan9.
  Even tsgo + a JS runtime port wouldn't get us Pi's TUI or extensions.
- **Not 9P-bridge-to-Mac.** Network-transparent, but requires Mac on,
  splits agent from box, no offline value when traveling.
- **Not acme-integrated.** User doesn't use acme. The terminal IS the
  desktop interface here.

pi9 = self-contained, native, modern-TUI, lives in the box.

## Architecture

```
┌──────────────────────────────────────────────────────┐
│ vtwin (libdraw, WinXP Luna palette, mxio titlebar)   │
│   ↓ cells diff stream + /sess/cons                   │
│ vts (terminal server, VT100 parser)                  │
│   ↓ ptyfork-equivalent: stdin/stdout/stderr          │
│ pi9 (Go binary, Bubble Tea TUI, ANSI to stdout)      │
│   ├─ provider clients (OpenRouter, Anthropic, …)     │
│   ├─ tool dispatch (read/write/run_rc/plumb/hget…)   │
│   ├─ session store /lib/pi9/sessions/                │
│   ├─ skills /lib/pi9/skills/                         │
│   └─ memory /lib/pi9/memory.md                       │
│   ↓ HTTPS (SSL_CERT_FILE=/sys/lib/tls/ca.pem)        │
│ Provider (Anthropic Claude / Kimi / OpenRouter)      │
└──────────────────────────────────────────────────────┘
```

pi9 itself knows nothing about plan9 graphics. It's a regular terminal
program — vts renders the ANSI, vtwin paints the cells. Same pattern as
running `pi` inside `wezterm` on a Mac.

## TUI: Bubble Tea + Lipgloss + Bubbles

User pick. The right one — it's the modern TUI stack and it has a
correctly-tagged plan9 path through `muesli/termenv`
(`termenv_other.go: //go:build js || plan9 || aix`).

Profile detection on plan9 returns `Ascii` (no escape detection), which
is fine — pi9 pins `termenv.WithProfile(termenv.ANSI)` because vts owns
the 16-color WinXP Luna palette and we want consistent rendering.

### Blocker: charmbracelet/x/term has no plan9 stub

Bubble Tea pulls in `github.com/charmbracelet/x/term` for raw-mode and
size queries. Their `term_other.go` explicitly **excludes plan9** from
the fallback build constraint:

```go
//go:build !aix && !darwin && !dragonfly && !freebsd && !linux && !netbsd
//       && !openbsd && !zos && !windows && !solaris && !plan9
```

So a clean `GOOS=plan9 GOARCH=amd64 go build` of any Bubble Tea program
fails:

```
# github.com/charmbracelet/x/term
term.go:5:2: undefined: state
term.go:10:9: undefined: isTerminal
term.go:17:9: undefined: makeRaw
...
```

**Verified on host 2026-05-16** with a minimal hello-world Bubble Tea
program against bubbletea v1.3.10 + x/term v0.2.1.

Fix: write `term_plan9.go` that wraps `golang.org/x/term` (which DOES
support plan9 — `golang.org/x/term/term_plan9.go` exists and provides
`MakeRaw/Restore/GetSize` for `/dev/cons` and `/dev/consctl`). About
60 lines. Two delivery paths:

1. **Local replace directive** in pi9's go.mod, vendored shim. Ships
   today, no upstream wait.
2. **Upstream PR** to `charmbracelet/x/term`. Lower-friction long-term
   but blocks on review. Do both: vendor first, PR in parallel.

Same problem may surface in `muesli/cancelreader` and one or two other
deps. Audit during Phase 0.

## On-disk layout

Following plan9 convention (`/lib/<svc>` for service data, `/sys/lib`
reserved for system):

```
/lib/pi9/
├── memory.md           # personal long-term memory (loaded every turn)
├── config              # provider keys, default model, theme
├── skills/             # markdown skills, loaded on demand
│   ├── mxio.md         # how mxio works (window manager)
│   ├── xena-panel.md   # taskbar
│   ├── vts-internals.md
│   ├── rc-idioms.md    # plan9 shell patterns
│   └── …
├── sessions/           # one file per session, JSON
│   ├── current         # symlink-ish: session id of active conv
│   ├── 2026-05-16-1422.json
│   └── …
└── prompts/            # user-saved reusable prompts ("/name" expands)
```

For session format we steal pi's tree-structured branching — single
JSON file per session, every message has a parent id, `/tree` navigates
to any point. Useful for "back up to before that bad tool call" without
losing the dead branch.

## Tool palette

Two tiers. Plan9-portable basics first, then plan9-native specials.

### Tier 1: portable (reuse from tinyxena)

```
read_file(path)             — read text file
write_file(path, content)   — create/overwrite
edit_file(path, find, repl) — exact-string edit (anchored)
run_rc(command)             — execute via rc shell, return stdout+stderr+rc
list_dir(path)              — ls equivalent
```

### Tier 2: plan9-native (new)

```
plumb(text, dst)            — send text through plumber to a port
                              (edit, image, web, etc.). plan9's
                              equivalent of "open with default app".
hget(url)                   — fetch URL via plan9's native HTTP client
walk(pattern)               — namespace traversal, finds files anywhere
                              mounted in the agent's namespace
mount(addr, mountpoint)     — bring a 9P service into the agent's view
                              (e.g. mount a remote box, then ls /n/box)
bind(src, dst)              — namespace overlay/union
ns()                        — dump current namespace (for self-discovery)
```

`plumb`, `walk`, `mount`, `bind`, `ns` are uniquely possible on plan9
and unlock things no Linux agent can do — letting the LLM reason about
the literal namespace it's living in. Real use case: "find the build
output across all mounted hosts and run the tests on whichever one
has gcc" — three tool calls, no SSH, no rsync.

### Out of scope (intentionally)

- Sub-agents — pi excludes them too. Use multiple pi9 instances in
  separate vts sessions, coordinate via shared `/lib/pi9/`.
- Background bash — vts handles this. Each session = one program. Want
  parallel? Open another vtwin.
- MCP — defer. Plan9's 9P is already the protocol MCP wishes it were.
  If we want external tools, expose them as 9P file servers and have
  pi9 mount them.
- Permission popups — run pi9 as your user, audit via session log.
  If we need sandboxing later, add a namespace-restricted variant
  using `rfork(RFNAMEG)`.

## Provider abstraction

OpenAI-compatible chat completions with tool calling. Already proven by
tinyxena. Three known-good provider endpoints:

```
Anthropic            api.anthropic.com/v1/messages (native shape)
OpenRouter           openrouter.ai/api/v1/chat/completions
Cerebras / Groq      OpenAI-compat, fast for cheap models
```

OAuth flows skipped for v0 — API keys via `/lib/pi9/config` only.
Plan9 has no system keychain; we encrypt-at-rest with `9 secstore`
in a later phase if anyone cares.

Streaming via SSE works on plan9's `net/http` — tinyxena confirmed
TLS 1.2+ negotiation against OpenRouter, just needs `SSL_CERT_FILE`
pointed at a CA bundle. Persistent connections, gzip — all standard
Go stdlib, all work.

## Skills system

Pi's progressive-disclosure model. Each skill is a markdown file with
YAML frontmatter:

```yaml
---
name: mxio
description: How mxio (the window manager) works
trigger: window manager, mxio, titlebar, decorations
---
```

pi9 sees the description list every turn (cheap, fits in the system
prompt). When the LLM asks for skill X, full body gets loaded. Same
pattern as Claude Code skills — proven.

We seed `/lib/pi9/skills/` from `wiki/concepts/` — the wiki **is** the
skill set, mostly. Symlink or copy-on-write. Authoring a skill =
authoring a wiki page = both benefit.

## Memory

`/lib/pi9/memory.md` is loaded into the system prompt on every turn.
Same shape as Claude Code's USER/MEMORY blocks: declarative facts that
should persist. pi9 has a `remember` tool that appends to it.

Soft cap ~4KB. Above that we surface "memory full" and ask the user
to prune or graduate facts into skills.

## TUI layout (v0)

```
┌─pi9────────────────────────────────────────── claude-sonnet ─┐
│ User: how does mxio's titlebar drawing work?                 │
│                                                              │
│ Assistant: looks at the title-painting flow, calls wmk →     │
│ wcontentrect → wdrawtitlebar. Let me check…                  │
│                                                              │
│   ▸ read_file(src/mxio/wind.c)        [42ms]                 │
│   ▸ read_file(src/mxio/winman.c)      [18ms]                 │
│                                                              │
│ Found it. wmk allocates a screen, then wcontentrect crops    │
│ off the top 22 px for the titlebar. The titlebar gradient    │
│ is painted by wdrawtitlebar which loops over rows…           │
│                                                              │
│                                                              │
├──────────────────────────────────────────────────────────────┤
│ > _                                                          │
└─[/help] [/model] [/clear] [/tree] [/skill]──── 12,400 / 200k ┘
```

Lipgloss-styled. WinXP-Luna-palette by default (matches vts). Status
bar shows model, token budget, session id.

Tool calls render as collapsible blocks — closed by default, expand
with arrow key. Streaming responses append in real time. Steering
(Enter mid-tool, like pi) defers — alt+enter sends after current tool.

## Build & deploy

Cross-compile from Mac, `hget` into the VM, drop into `/bin/amd64/pi9`
once a writable disk install exists. For now `/tmp/pi9` is fine.

```
mkfile (in src/pi9/, plan9 mkfile)
$O.out: main.go provider.go tools.go tui.go skill.go session.go
  GOOS=plan9 GOARCH=amd64 go build -o $target ./...
```

OR more sensibly: build on the Mac (faster), serve via http.server,
hget into the VM. Established pattern, see tinyxena README.

## Phasing

| Phase | Goal | Effort |
|---|---|---|
| **0** | Unblock Bubble Tea on plan9: write `term_plan9.go` shim for `charmbracelet/x/term`, audit other deps (`muesli/cancelreader`, etc.) for same problem, set up vendoring | 1-2 days |
| **1** | Bubble Tea "hello pi9" running in vts: model/update/view, alt-screen, basic chrome. Cross-compiles, runs in VM | 2-3 days |
| **2** | Provider client + streaming response renderer. Single hardcoded provider. No tools yet | 3-4 days |
| **3** | Tool dispatch: read_file, write_file, run_rc. Tool blocks render in TUI | 3-4 days |
| **4** | Sessions (persist+restore), skills loader, memory.md | 3-4 days |
| **5** | Plan9-native tools: plumb, hget, walk, mount, ns. Where pi9 stops being a port and starts being a plan9 thing | 3-4 days |
| **6** | Slash commands (`/model`, `/clear`, `/tree`), favorites cycling, polish, themes | 2-3 days |

Total: 3-4 weeks of focused work. All bounded engineering, no research.

Stretch: package as a mxio-launchable app with its own xena-panel
status indicator ("pi9: thinking" / "pi9: tool: run_rc"). Phase 7.

## Risks and unknowns

1. **Bubble Tea's plan9 path beyond x/term.** termenv has a plan9 file,
   but bubbletea's input handling (`muesli/cancelreader` for cancellable
   stdin reads) and its event loop use platform-specific code. Likely
   patchable. Won't know until Phase 0 fixes x/term and we hit the next
   error.
2. **vts VT100 coverage.** pi9 will emit alt-screen, cursor positioning,
   SGR, OSC title — all listed in [[vt100-parsing]] as supported.
   Mouse reporting, bracketed paste are NOT supported in vts. Make
   sure Bubble Tea doesn't *require* mouse — it should be opt-in
   (`tea.WithMouseCellMotion`), so don't enable it.
3. **Go stdlib gotchas on plan9.** `os/exec` semantics differ (rfork
   not fork); `signal.Notify` doesn't behave like Linux (plan9 has
   notes not signals — Go translates partially). Tinyxena hit none
   of these, but pi9's broader surface might. Each is fixable.
4. **TLS root certificates on 9front.** Live ISO has no `/sys/lib/tls/ca.pem`.
   Need a disk install (planned anyway) or ship `ca.pem` alongside
   the binary with `SSL_CERT_FILE` pointing at it. Tinyxena uses the
   latter.
5. **No JIT, no Go runtime tuning for plan9.** Performance fine for an
   agent (network-bound). Don't try to put a vector index here.

## Decisions deferred

- **Theming beyond Luna.** v0 ships Luna only. Theme system later.
- **Voice.** No TTS/STT on plan9. Defer indefinitely.
- **MCP.** See "out of scope". Mount 9P services if extension needed.
- **Embedding/RAG.** No vector index yet. Skills + memory cover most
  cases. If we want, embed via `hget` to a remote embedding service
  rather than running BLAS on plan9.

## See Also

- [[plan9-namespaces-for-agents]] — **the killer-feature page**: why
  per-process namespaces fundamentally change what an LLM agent can
  do vs Linux/Mac agents. Start here for the conceptual case.
- [[vt-architecture]] — vts the terminal server pi9 runs inside
- [[vt-9p-namespace]] — vts's wire protocol (cells/cons/ctl)
- [[vt100-parsing]] — what escape sequences vts handles (TUI vocabulary)
- [[vtwin-typography]] — what font vtwin renders pi9 in (spoiler: anything we want)
- [[build-toolchain]] — cross-compile from Mac, hget into VM
- [[llm-porting-workflow]] — methodology for plan9 ports (used here)
- `~/Projects/plan9-agent/README.md` — tinyxena, the proof-of-concept
  pi9 builds on
- <https://pi.dev/> — the agent whose shape pi9 mirrors
- <https://github.com/charmbracelet/bubbletea> — TUI framework
