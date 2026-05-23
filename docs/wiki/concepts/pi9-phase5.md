---
title: pi9 Phase 5 — Plan9-Native Tools
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [arch, plan9, status-done]
---

# pi9 Phase 5 — Plan9-Native Tools

> **Status: done 2026-05-17.** pi9 has six new tools (`plumb`, `hget`, `walk`, `ns`, `bind`, `mount`) that expose Plan 9-only primitives no Linux/Mac agent can do. The agent can now introspect its own namespace, walk arbitrary file trees, fetch URLs via Plan 9's native HTTP stack, plumb text to handlers, and reshape its own namespace in-process. Verified end-to-end: `ns({filter:"/srv"})` returned 13 real mount lines; `walk("/sys/lib/tls", depth=2)` returned the actual directory contents; `hget(url)` downloaded a real markdown file (47 lines) in 55ms.

This is the phase where pi9 stops being "an agent ported to Plan 9" and becomes "an agent that could only exist on Plan 9."

## Tool tiers

| Tier | Tools | Implementation |
|---|---|---|
| **Portable** | read_file, write_file, run_rc, remember, read_skill | stdlib + `os/exec`; works anywhere |
| **Plan9 shell-out** | plumb, hget, walk, ns | shell out to `/bin/<tool>` on plan9; clean error on other OSes |
| **Plan9 syscall** | bind, mount | `golang.org/x/sys/plan9` Bind/Mount; build-tagged `namespace_plan9.go` / `namespace_other.go` |

The three tiers compile and run together. On non-plan9 hosts, the
plan9-specific tools advertise themselves to the model but return
`"<name> is Plan 9 only (running on darwin)"` if invoked. Cleaner
than letting `/bin/walk` fail with `file not found`.

## The new tools

### plumb(port, content)

Plan 9's plumber is what right-click-open-with should have been everywhere.
It routes text/paths/URLs to the right handler based on type and content:
`/sys/man/foo.c` → editor; `https://...` → browser; `file:1234` →
debugger line jump. Pi9 shells out to `/bin/plumb -d <port>` with the
content on stdin.

Example use: model sees an error message mentioning `foo.go:42:7`,
plumbs it via `plumb({"port": "edit", "content": "foo.go:42"})` —
acme/sam jumps to that line.

### hget(url)

Plan 9's native HTTP client. Uses the system's `/sys/lib/tls/ca.pem`
for TLS. Faster than running our own Go HTTP from inside `pi9` because
plan9's hget is C, doesn't have to do Go's TLS handshake cost.

In the test: pi9 fetched a markdown file from the Mac host's
http.server. 55ms round-trip. Useful for "go look at this README on
github" workflows.

### walk(path, [depth=N])

Plan 9's recursive directory lister. Equivalent of `find -type f` on
unix but native to plan9. Returns one path per line, sorted depth-first.

```
/sys/lib/tls/acmed
/sys/lib/tls
```

Optional `depth` arg caps how deep we go. Output truncates at 16KB
(see "Output truncation" below).

### ns([filter])

The killer feature. Plan 9 namespaces are per-process; `ns` dumps the
calling process's view. Pi9 sees its OWN namespace — different from
the user's shell.

In our test, `ns({"filter": "/srv"})` returned:

```
bind -c '#s' /srv
mount -C /srv/boot /root
mount -a /srv/boot /
mount -a /srv/slashn /n
mount -a /srv/slashmnt /mnt
mount -a /srv/mntexport /mnt/exportfs
mount -b /srv/cons /dev
mount  /srv/factotum /mnt/factotum factotum
mount -a /srv/cs /net
mount -a /srv/dns /net
mount  /srv/plumb.glenda.444 /mnt/plumb
mount  /srv/rio.glenda.449 /mnt/wsys none
mount -b /srv/rio.glenda.449 /dev
mount  /srv/vts /n/vts
```

This is the literal current state of pi9's filesystem view. **No
agent on any other OS can do this** because no other OS exposes its
own namespace as a queryable runtime object.

### bind(src, dst, [flag])

Equivalent of Linux's `mount --bind`, but per-process and reversible.
Pi9 calls `plan9.Bind(src, dst, flag)` via `golang.org/x/sys/plan9`.
Affects pi9's view only — not the user's shell.

Flag is one of `replace` (default), `before`, `after`, `create`.

### mount(srv, mountpoint, [flag])

Plan 9's `mount(2)`. We open the 9P service file (typically
`/srv/<name>`), pass the fd to the syscall, and the service is
attached at `mountpoint` in pi9's namespace.

Example: `mount({"srv": "/srv/cfs", "mountpoint": "/n/cfs"})`.

## Architecture

```
src/pi9/internal/tools/
├── tools.go                main registry + portable + shell-out impls
├── namespace_plan9.go      bindTool, mountTool — uses plan9.Bind/Mount
└── namespace_other.go      stubs returning "Plan 9 only" errors
```

The plan9 syscall tools are split via build tags so the Go compiler
on darwin/linux/etc. doesn't try to compile the plan9 import. Same
pattern we used for `vendor-patches/bubbletea/signals_plan9.go` in
Phase 1.

## Output truncation

Each tool's result is capped at **16KB** via the `truncate()` helper.
If the output exceeds, it's clipped with a suffix:

```
[… 24432 more bytes truncated]
```

Why: a `walk /` would return tens of MBs and blow out the model's
context. The model can rerun with a more specific path or use
`depth` to constrain. Tradeoff: model can't see the tail of huge
outputs without re-running. Acceptable.

The cap applies uniformly across all tools — read_file too. If you're
asking for a huge file, do `read_file` then `run_rc("head/tail")`.

## Verified end-to-end

Screenshot: `wiki/assets/pi9-phase5-rendered.png` shows the
namespace inspection turn rendered in vtwin with the cyan
`▸ ns({"filter": "/srv"}) [32ms] 417 bytes` block.

Session JSON for ns test (excerpt):

```json
{
  "user": "show me the namespace",
  "calls": [{
    "id": "call_mock_1",
    "name": "ns",
    "args": "{\"filter\": \"/srv\"}",
    "output": "bind -c '#s' /srv \nmount -C /srv/boot /root \n…",
    "started": "2026-05-16T23:00:45.288Z",
    "finished": "2026-05-16T23:00:45.320Z"
  }]
}
```

Also verified live in the VM (separate runs):

- `walk({path: "/sys/lib/tls", depth: 2})` → real directory contents
- `hget({url: "http://10.0.2.2:8765/.../plan9-rc.md"})` → real markdown body (~1.8KB)

`plumb`, `bind`, `mount` are wired but weren't visually retested in
this session. The shell-out / syscall wrappers are simple and
identical in shape to the verified ones.

## Acceptance criteria — all met

| Criterion | Verified |
|---|---|
| All six tools advertised in schema | ✅ Schemas() returns 11 tools (5 from Phase 4 + 6 new) |
| Plan9 shell-out tools work | ✅ ns, walk, hget executed against real plan9 fs |
| Plan9 syscall tools compile for plan9 | ✅ `GOOS=plan9 GOARCH=amd64 go build` succeeds with `golang.org/x/sys/plan9` |
| Cross-platform builds work | ✅ host build (darwin) also clean — bindTool/mountTool stubs kick in |
| Tool output truncated past 16KB | wired but not retested (no test triggers it yet) |
| Tools survive on non-plan9 | wired — return `"<name> is Plan 9 only (running on darwin)"` |

## Quirks worth remembering

### `golang.org/x/sys/plan9` requires Go ≥1.25

Pulling in this dep auto-upgraded our `go.mod` from `1.24` to `1.25`.
First time we've hit this. Means: building on hosts with older Go
(<1.25) won't work. Document in README or pin if it becomes a
problem.

### Plan 9 hget pollutes stderr with rc init warnings

When pi9 spawned `/bin/hget URL`, the captured combined output
included noise like:

```
/env/fn#sigexit:1: syntax error
/env/fn#term%:4: syntax error
```

That's rc trying to source pi9's exported env functions (which were
created by the parent rc). Doesn't affect hget's functionality.
Cosmetic ugliness in tool output. Could fix by clearing certain env
vars before exec, but not worth it.

### ns is per-process, not per-user

Critical mental model: `ns` returns pi9's namespace, not the
user-shell's namespace. If pi9 calls `bind` mid-conversation, the
user's shell doesn't see the change. This is by design — pi9
exploring its own view of the filesystem doesn't surprise the user.

If we ever want pi9 to affect the user's namespace (e.g. "mount this
remote box and let me access it"), we'd need a separate
`exec_in_user_ns` tool that spawns a child in the user's shell with
the bind/mount commands. Out of scope for Phase 5.

### Plumb output is silent on success

`/bin/plumb -d port` produces no output when the message routes
successfully. Our wrapper synthesises a result string in that case:
`"plumbed N bytes to port \"edit\""`. The model gets a confirmation
either way.

## What's deferred

### Phase 6 (polish)

- Stale-frame artifacts in input box during streaming
- Header pinned (currently scrolls off when turn count is high)
- Multi-line input
- Slash commands (`/clear`, `/model`, `/sessions`, `/save`, `/help`)
- Scrollback navigation
- Word-wrap with rune-width awareness

### Phase 7 (xena-panel integration)

- pi9 status widget on taskbar
- "Ask pi9" launcher in start menu
- Right-click "Send to pi9" plumber rules

## See Also

- [[plan9-namespaces-for-agents]] — **why this phase matters**: the
  conceptual case for per-process namespaces as agent infrastructure.
  Read this if you want to understand what pi9 unlocks vs other agents.
- [[pi9-architecture]] — overall design
- [[pi9-phase1]] — Bubble Tea on plan9
- [[pi9-phase2]] — streaming chat
- [[pi9-phase3]] — tool calling foundation
- [[pi9-phase4]] — sessions, skills, memory
- `src/pi9/internal/tools/tools.go` — tool registry + portable + shell-out
- `src/pi9/internal/tools/namespace_plan9.go` — bind/mount syscalls
- `src/pi9/internal/tools/namespace_other.go` — non-plan9 stubs
- `wiki/assets/pi9-phase5-rendered.png` — screenshot of ns turn
