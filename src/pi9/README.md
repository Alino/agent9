# pi9

Plan9-native LLM coding agent. See `wiki/concepts/pi9-architecture.md` for design.

## Status: Phase 5 — Plan9-native tools ✅

Verified 2026-05-17. pi9 now has eleven tools, six of which expose
Plan 9-only primitives no agent on any other OS can use: introspect
its own namespace, walk file trees with /bin/walk, fetch URLs via
plan9's native hget, plumb text to handlers, and reshape its own
namespace via Bind/Mount syscalls.

Phase writeups:
- `wiki/concepts/pi9-phase1.md` — Bubble Tea on plan9
- `wiki/concepts/pi9-phase2.md` — streaming chat
- `wiki/concepts/pi9-phase3.md` — tool calling
- `wiki/concepts/pi9-phase4.md` — sessions, skills, memory
- `wiki/concepts/pi9-phase5.md` — plan9-native tools (this phase)

## Tools (11 total)

### Portable

| Tool | Description |
|---|---|
| `read_file(path)` | Read a file |
| `write_file(path, content)` | Write a file (creates dirs, overwrites) |
| `run_rc(command)` | Run a shell command via rc on plan9 / sh elsewhere |
| `remember(content)` | Append a durable fact to `memory.md` |
| `read_skill(name)` | Load the full body of a named skill |

### Plan 9-native (shell-out)

| Tool | Description |
|---|---|
| `plumb(port, content)` | Route text via plan9 plumber (open with handler) |
| `hget(url)` | Fetch URL via plan9 native HTTP client |
| `walk(path, depth)` | Recursive directory listing |
| `ns(filter)` | Dump the current namespace (per-process!) |

### Plan 9-native (syscall)

| Tool | Description |
|---|---|
| `bind(src, dst, flag)` | Bind path into pi9's own namespace |
| `mount(srv, mountpoint, flag)` | Mount a /srv 9P service into namespace |

All plan9-native tools return `"<name> is Plan 9 only (running on $OS)"`
when invoked from a non-plan9 host — keeps cross-platform builds clean.

Tool output is capped at 16KB per result; longer is truncated with
a `[… N more bytes truncated]` suffix.

## On-disk layout

```
$home/lib/pi9/        (plan9)   or    $HOME/.pi9/   (unix)
  $PI9_HOME also honored if set
├── memory.md              long-term facts
├── skills/                on-demand markdown skills
│   ├── plan9-rc.md
│   └── vts-cells.md
└── sessions/
    ├── current
    ├── 2026-05-17T03-00-41.json
    └── ...
```

## Build

```sh
./scripts/build-pi9.sh              # cross-compile plan9/amd64
./scripts/build-pi9.sh --serve      # cross-compile + start http.server
./scripts/build-pi9.sh --host       # also build host binary

cd src/pi9 && mk                    # build inside plan9
```

Requires Go ≥ 1.25 (golang.org/x/sys/plan9 minimum).

## Run

```sh
OPENROUTER_API_KEY=sk-or-... /tmp/pi9             # resume current session
OPENROUTER_API_KEY=sk-or-... /tmp/pi9 -new        # fresh session
OPENROUTER_API_KEY=sk-or-... /tmp/pi9 -session ID # load specific session
```

Mock for testing (no API key needed):

```sh
python3 testtools/mock-openrouter.py &
OPENROUTER_API_KEY=mockkey                   \
OPENROUTER_API_URL=http://10.0.2.2:8766/chat \
/tmp/pi9
```

Mock branches recognize: "remember", "skill", "namespace"/"ns ",
"walk"/"tree"/"explore", "hget"/"fetch"/"url", "plumb", "ls"/"tmp",
"read file", otherwise canned text response.

## Keys

- Type freely, Enter to submit
- ctrl-u clears input
- left/right/home/end navigate
- ctrl-c (or esc) cancels streaming
- ctrl-c again, or ctrl-d, quits pi9

## Files

```
.gitignore
README.md                this file
go.mod / go.sum
main.go                  Bubble Tea model, agent loop, system prompt
mkfile                   plan9 mk(1) build
internal/
  chat/chat.go           History, Turn, ToolInvocation + JSON tags
  provider/openrouter.go OpenAI-compatible streaming + tool_call assembly
  store/store.go         Home, sessions, skills, memory
  tools/
    tools.go             portable + shell-out tools
    namespace_plan9.go   bind/mount via plan9 syscalls
    namespace_other.go   stubs for non-plan9
testtools/
  README.md
  celldump.c             cell-level verification utility (plan9 C)
  mock-openrouter.py     local SSE mock with tool-call branches
  seed-skills/           example skill content
    plan9-rc.md
    vts-cells.md
vendor-patches/bubbletea/  plan9 shim files
```
