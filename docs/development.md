# Development notes

This file explains how to develop ON the agent9 source from a
non-Plan-9 host (almost certainly a Mac or Linux box).

## Layout

```
agent9/
├── src/
│   ├── mxio/        — window manager (C, libdraw)
│   ├── xena-panel/  — taskbar (C, libdraw)
│   ├── launcher/    — start menu (C)
│   ├── vts/         — terminal session 9P server (C)
│   ├── vtwin/       — libdraw frontend for vts (C)
│   ├── pi9/         — LLM agent (Go, Bubble Tea)
│   └── xfiles/      — file manager stub (C)
├── docs/            — public docs (this file, ARCHITECTURE.md, ...)
├── release/         — runner scripts + the prebuilt qcow2
├── tools/           — helpers (vm-rc, p9sync, etc.)
└── wiki/            — concept pages, schema, log
```

## The dev loop

1. Edit code on the host.
2. Push to the VM via HTTP (host runs `python3 -m http.server` in the
   agent9 root, VM uses `hget`). Or use `tools/p9sync` for a more
   structured shuttle.
3. Build inside the VM via `mk` in `/sys/src/cmd/<name>/`.
4. Test by killing the running daemon and re-running.

For pi9, Go cross-compiles from the host:

```bash
cd src/pi9 && GOOS=plan9 GOARCH=amd64 go build -o pi9.plan9.amd64 .
# then push pi9.plan9.amd64 to /amd64/bin/pi9 in the VM
```

## Reaching the VM

The release runners forward host ports:

| Host | Guest | Purpose |
|---|---|---|
| 2222 | 22 | ssh (no password — dev only) |
| 1717 | 17010 | listen1 rc shell (one-shot commands) |
| 1564 | 564 | 9P export |
| 53692 | 53692 | OAuth callback for pi9 |

Listen1 lets you run rc commands from the host without ssh:

```bash
echo 'ls /amd64/bin' | nc -w 5 127.0.0.1 1717
```

It's flaky on slow VMs. Retry with longer timeouts if a command
silently returns empty output.

For interactive work, ssh:

```bash
ssh -p 2222 glenda@localhost
```

## Cross-compiling pi9

```bash
cd src/pi9
GOOS=plan9 GOARCH=amd64 go build -o /tmp/pi9 .
# push:
echo "hget http://10.0.2.2:8765/pi9 > /tmp/pi9 && chmod +x /tmp/pi9" | nc -w 30 127.0.0.1 1717
# install:
echo "cp /tmp/pi9 /amd64/bin/pi9" | nc -w 5 127.0.0.1 1717
```

The first build requires patching `charmbracelet/x/term` for plan9
support — see `src/pi9/internal/shim/` and the long story in
`wiki/concepts/pi9-architecture.md`.

## Building C components

Cross-compilation isn't possible (no hosted Plan 9 toolchain). Build
in the VM:

```bash
ssh -p 2222 glenda@localhost
cd /sys/src/cmd/mxio
mk install         # builds and copies to /amd64/bin/mxio
```

To reload mxio without rebooting:

```
echo kill > /proc/$(pgrep mxio)/ctl
# wait, then re-launch from a fresh terminal (rio has to be in scope)
```

## Wiki

Architecture decisions, gotchas, and per-phase write-ups live under
`wiki/`. Start with `wiki/index.md`. The wiki is in plain markdown
with YAML frontmatter — works as-is in Obsidian, also renders fine on
GitHub.

When you add a new architectural decision or work around a Plan 9
quirk, add a `wiki/concepts/<topic>.md` page. The wiki is the
project's long-term memory.

## Testing

For C components with pure logic (vts cell buffer, VT100 parser),
`src/vts/test/run_tests.sh` runs the tests against macOS clang via
the `compat.h` shim. ~49 tests pass on macOS, ~5 integration scripts
pass in the VM.

For pi9, `src/pi9/testtools/mock-openrouter.py` is a deterministic
mock provider that returns canned responses based on prompt keywords.
Use it for offline TUI testing.

## Logs and state

The pi9 agent writes to predictable locations:

```
/usr/glenda/lib/pi9/sessions/   — JSON sessions
/usr/glenda/lib/pi9/skills/     — skill markdown
/usr/glenda/lib/pi9/config      — provider keys, default model
/usr/glenda/lib/pi9/auth.json   — OAuth tokens (gitignored)
/tmp/new-pi9.log                — launch trace
```

vtwin writes to:

```
/tmp/vtwin.log                  — render log
/tmp/vt-key.log                 — keyboard trace (if -K flag)
/tmp/vt-mouse.log               — mouse trace (if -M flag)
```

Reading these files from the host is usually faster than screenshotting
the VM.

## Don't

- Don't propose Rust ports. No Plan 9 target for rustc.
- Don't propose Python in the runtime. Python 3 isn't ported.
- Don't bring Linuxisms into the C (epoll, pthreads, signalfd). Plan 9
  has threads(2), channels, alt(), 9P, rfork. Use them.
- Don't malloc/free. Use mallocz, sysfatal, the libthread proc model.
- Don't propose nvim/vim. Use acme, or `9vim` (a port that exists but
  is mostly a curiosity).

## See also

- `release/RUNNING.md` — how a non-developer runs the release image.
- `wiki/concepts/pi9-architecture.md` — pi9 design.
- `wiki/concepts/vt-architecture.md` — vts/vtwin design.
- `wiki/concepts/plan9-namespaces-for-agents.md` — why this stack
  matters for LLM agents specifically.
