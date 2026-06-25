# pi9 as a 9P Filesystem — "the agent is a set of files"

> Status: **idea / not started.** Captured 2026-06-25 from a design discussion.
> We will get to this later. This is the thesis-proving next step after the
> python9 + node9 ports made the platform *usable*; this is about making it
> *compelling*.

**Goal:** Expose a running pi9 agent as a mountable 9P filesystem, so the agent
is driven and observed entirely through file operations — `echo` to send a turn,
`cat` to stream the reply, `cat ctl` to inspect state. Mountable from the host,
another machine, or a parent agent's namespace.

```
/n/pi9/
  ctl          # write commands: new | resume <id> | model <name> | cancel
  prompt       # write a line here = send a turn to the agent
  output       # read here = stream the assistant reply (blocking, like cons)
  status       # read = current state (idle/streaming/model/ctx usage)
  session/     # the conversation tree as files (turns, branches) — browsable
  tools/       # each tool a file; writing args invokes it, read = result
```

## Why this, why now (the evidence is from 2026-06-24/25)

The whole node9-FP-fix session was a natural experiment in "what's reliable on
this platform," and the result was unambiguous:

- **Every time 9P was the interface, it just worked.** Reading pi9's screen via
  `/n/vts/<s>/cells`, driving it via `cons`, inspecting via `ctl`, reading
  scrollback via `scroll` — rock solid, remotable, composable.
- **Everything bolted on the side was flaky.** listen1 binary transfer
  (truncated a 2.5 MB write), ad-hoc HTTP servers (wouldn't stay bound), the
  release upload (wedged for 41 min). These cost an entire session.

That asymmetry is the platform telling us where its grain runs. The bet: lean
*into* 9P, away from bolted-on scaffolding — and the most Plan-9-native thing we
can do is make the agent itself a filesystem.

## What it proves / why it pays for itself

1. **Proves the thesis.** "Agent as files" is the strongest possible answer to
   *why Plan 9 at all*. If mounting and driving an agent through files feels
   better than an HTTP/RPC agent API — observable, composable, remotable for
   free — that's the differentiator. If it doesn't, we learned it cheaply.
2. **Kills the dev-loop fragility immediately.** Mount `/n/pi9` from anywhere
   and drive it with `echo`/`cat`. No more flaky listen1/HTTP scaffolding — the
   agent becomes a first-class citizen of the OS it lives in.
3. **The hard multi-agent parts fall out of namespaces:**
   - **Sub-agents** = child processes with their own `/n/pi9` view.
   - **Capability sandboxing** = bind only the chosen `tools/` and files into a
     child's namespace; it literally cannot see what you didn't bind.
   - **Remote control / observability / fan-out** = "it's just files," the
     property Linux makes you reinvent each time.

## Architecture

- A 9P file server (`lib9p`, like `vts`) wrapping the pi9 agent loop. The agent
  is Go (bubbletea TUI today); the 9P surface is a *new front-end* to the same
  core, so the TUI and the filesystem are two clients of one engine — exactly
  how `vts` has `cells` (libdraw client) and `scroll` (dumb client) over one
  buffer. Reuse that split.
- `output`/`prompt` behave like `cons`: blocking reads deliver streamed tokens;
  writes inject turns. `ctl` is the control channel (parallel to vts's ctl).
- Sessions outlive clients (disconnect = unmount, reconnect = remount), same as
  vts sessions outliving vtwin windows (we saw this firsthand: killing vtwin
  left the pi9 session alive and re-attachable).
- Post to `/srv/pi9`; `mount /srv/pi9 /n/pi9`. From the host, reach it the same
  way we reach vts today (9P over the qemu/LAN forward).

## Companion track: self-hosting ("the agent maintains its own box")

The true "agents living in Plan 9" milestone is pi9 running its own
edit → build → test → run loop *inside* cirno, so it can fix the next
node9/python9 bug itself instead of being maintained from the host (as we did
all session). This rhymes with the 9P work: once the agent is drivable as files,
having it drive *itself* — or a sub-agent — is a short hop. Sequence the two
together; the 9P surface is much of what self-hosting needs anyway.

## Minimal first step (do this to validate fast)

Stand up a **minimal `/n/pi9`** with just `ctl` + `prompt` + `output` (skip
`session/`, `tools/` for v0). Mount it from the host. Drive a full pi9 exchange
entirely through file ops. Single question to answer: **does this feel better
than what we did today?** That validates or kills the whole direction in one
small build.

## Out of scope (for the first cut)

- Authenticated/encrypted 9P — rely on existing factotum/forwarding, like vts.
- Reworking the bubbletea TUI — it stays as a parallel client of the same core.
- Multi-agent orchestration UX — prove single-agent-as-files first; sub-agents
  follow naturally from namespaces once the surface exists.
- Package-coverage / `npm help` polishing — explicitly NOT next; that's
  runway-polishing. The ports are good enough to prove things now.

## Reference reading before starting

- `docs/plans/2026-05-16-vt-console-daemon.md` — the vt/vts design; this plan is
  the same idiom applied to the agent instead of a terminal. Reuse its lib9p +
  one-core-many-clients structure.
- `src/vts/` — working reference for a Plan 9 9P file server with `ctl`/`cons`/
  `cells`/`scroll` and client-outliving sessions.
- `src/pi9/` — the agent core to wrap (Go; bubbletea front-end is one client).
- Memory: `node9-fp-divzero-trap` and the listen1/HTTP fragility notes — the
  empirical case for "lean into 9P."
