---
title: Plan 9 Namespaces for LLM Agents
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [plan9, 9p, arch]
---

# Plan 9 Namespaces for LLM Agents

> Why pi9 having `ns`, `bind`, `mount`, `plumb`, `walk`, `hget` isn't
> just "more tools" — it's a fundamentally different relationship
> between the agent and its environment than any Linux/Mac agent has.

There are two ways to read "the agent can introspect its namespace."
The narrow read is "pi9 can see its mount table." That's not what
matters. The deep read is **the agent's environment is itself a
queryable, manipulable runtime object** — and that flips the
relationship between an agent and its surroundings in ways no other
OS allows.

## The frame: per-process namespaces

On Linux, "the filesystem" is a singular shared thing managed by the
kernel. Every process sees the same `/etc/passwd`, the same `/home`,
the same `/var`. `mount -t nfs …` affects every process. Per-process
mount namespaces exist (`unshare(CLONE_NEWNS)`) but are hostile to
use, require root, and aren't the primitive — they're a workaround.

On Plan 9, **every process has its own private filesystem view from
birth**. You don't acquire it via syscall; you have it. When pi9
forked from rc, it inherited rc's view. Pi9 can sculpt that view
([[../../src/pi9/internal/tools/namespace_plan9.go|bind, mount,
unmount]]) and only pi9 sees the changes. The user's shell is
untouched. The kernel doesn't need to be told about any of this —
it's a per-proc data structure, not a global registry.

`ns(1)` dumps that data structure as text. **Every line is a valid
rc command that would reproduce that mount.** The namespace IS the
program that built it. State and code are the same artifact.

## Everything is a file — including the file system itself

Plan 9 takes "everything is a file" further than Linux ever did:

| Resource | Linux | Plan 9 |
|---|---|---|
| Auth credentials | keychain, libsecret | `/mnt/factotum/ctl` |
| DNS | resolver library | `/net/cs` |
| Network connections | socket() syscall | `/net/tcp/N` directories |
| Window system | X11/Wayland protocol | `/dev/draw` + `/mnt/wsys` |
| Remote machine | ssh client | `mount /srv/remote /n/remote` |
| Plumber routing | n/a | `/mnt/plumb/send` |

For an LLM agent, this is enormous: **every service the agent might
want to use is reachable via file I/O**. No client libraries. No
protocols. No serialization layers. Read a file to query state, write
a file to act.

## What "introspection" means specifically

When pi9 runs `ns({"filter": "/srv"})`, it reads a single virtual
file. Not `/proc/mounts` (kernel's global view), not
`/proc/self/mountinfo` (kernel's per-process view but still a kernel
construct). It reads ITS OWN view, computed by the same kernel
structure that does name resolution every time pi9 calls `open()`.

**The thing being queried IS the thing being used.** That's already
different from any other OS. The shift comes from what becomes
possible.

## Capability 1: self-diagnosis without external context

Other agents debug filesystem access by trial and error or by asking
the user. "I can't read /home/alex/.ssh/id_rsa" → the user has to
explain permissions, sudoers, container mounts, AppArmor profiles,
whatever. The agent operates on a black box.

Pi9 reads its `ns` and sees exactly why something fails. "/n/work
isn't mounted in my view" is the answer. It's also the prescription:
`mount /srv/work /n/work` and try again. The agent diagnoses its own
failures without privileged knowledge.

## Capability 2: capability scoping that actually works

Most "agent sandboxing" is bolted on: containers, seccomp, chroot,
permission prompts. These work by *restricting* a global filesystem
to a subset. Plan 9 inverts it: there is no global filesystem to
restrict. You CONSTRUCT the agent's view from primitives.

`rfork(RFNAMEG)`, then bind only `/tmp`, then exec pi9. The agent
now physically cannot read `/home` — not because access denied,
because `/home` doesn't exist in its world. There's no `..` escape,
no symlink trick, no procfs trapdoor. **Sandboxing via construction
is fundamentally more robust than sandboxing via restriction.**

Future-state: skill-scoped namespaces. When pi9 loads the
"github-pr-workflow" skill, it auto-binds `~/.config/gh` into its
namespace. When the skill is done, it unbinds. The agent's
capabilities are scoped to what it's currently doing — automatically,
by virtue of how Plan 9 works, no policy file needed.

## Capability 3: service composition as filesystem operations

On Plan 9, services are file servers. Auth is `/srv/factotum`. DNS is
`/srv/cs`. The window system is `/srv/rio.glenda`. Plumber routing is
`/srv/plumb`. Each one a 9P-speaking process. To use a service, you
mount it. To query it, you read files in it. To act, you write files.

Pi9 already mounts `/srv/vts` to read its own session's size from
`/n/vts/<sid>/ctl` (see [[pi9-phase4]]). That's the same primitive
that lets it mount a remote machine's filesystem, an in-memory
database, a plumber for routing, a synthesized API server.
**Everything composes the same way.**

Linux LLM agents need a different client library per service: an SSH
client, a Redis client, an HTTP client, a database driver. Each with
its own auth, its own serialization, its own error mode. **Pi9 needs
one tool — `mount` — and everything becomes file I/O.**

## Capability 4: remote = local, with no special tooling

```
srvssh remotehost
mount /srv/remotehost /n/remote
read_file("/n/remote/sys/log/cpu")
```

The agent didn't acquire a new tool, didn't learn a new protocol,
didn't pass auth state around. It mounted a machine. Now that
machine's filesystem is part of pi9's namespace. Operating on remote
files is identical to operating on local files.

For an agent loop, this is enormous. "Read the production logs"
doesn't mean "remember the SSH config, escape it correctly into a
shell command, parse the result of `ssh user@host cat
/var/log/…`". It means `walk /n/prod/sys/log`. The same tool that
lists `/tmp` lists the remote machine. **The model doesn't need to
learn a different mental model for "remote work."**

## Capability 5: reproducible environments

Save pi9's `ns` output to a file. Hand it to another pi9 instance
running on another machine. That instance's namespace is now
identical. Same view, same mounts, same composition. Agent state —
the part that matters for "what can I see and do" — is portable as
text.

Today's agents don't have this. Cursor on my Mac sees a different
filesystem than Cursor on yours. Even with identical project files,
the surrounding environment (which Homebrew, which Python, which CA
bundle, which keychain) drifts. **Pi9 can hand its environment over
verbatim. Replay an agent run with the exact same world.**

## A concrete comparison

> "Pi9, check if our build server is reachable."

A Linux/Mac agent would: parse some config, run
`ssh -o BatchMode=yes user@build "echo ok"`, parse the result, handle
connection failures, maybe fall back to ping, maybe check DNS. The
whole thing is a constellation of shell-out tools held together by
string-parsing.

Pi9, on a properly-set-up Plan 9: `ns({"filter": "build"})`. If
`/srv/build` is in the output, it's already attached. If not:
`mount tcp!build!564 /n/build`. Now `walk /n/build/sys` lists the
remote machine's tree. **One conceptual tool. Two file ops. No client
libraries.**

The agent isn't translating between "local fs" and "remote machine"
mental models. There's just one model: things-in-the-namespace.

## What this unlocks for pi9 specifically

Phase 5 wired the primitives. The interesting work isn't more tools —
it's getting the model to think in namespace terms. When that lands:

- **"Mount the project's git repo as a namespace and let me explore
  it"** — pi9 binds a clean view, runs `walk`, finds files, edits,
  unmounts.

- **"Work on the production box for a bit"** — pi9 srvssh's in,
  mounts the remote, operates as if local, unmounts when done. User's
  shell never touched the remote.

- **"Sandbox yourself to /tmp/scratch and try this risky thing"** —
  pi9 rforks a namespace with only `/tmp/scratch` bound, no `$home`
  visible, no `/srv` accessible. Experiments are physically
  reversible.

- **"Two pi9 agents working on different parts of a project, sharing
  notes via a 9P file server"** — they coordinate by reading and
  writing files in a shared namespace. No protocol layer. Files are
  the protocol.

- **"Give me a namespace dump of your current view so I can hand it
  to a colleague to debug"** — pi9 saves `ns` to a file, colleague
  runs it, they're now in the same world.

The bind/mount tools in [[pi9-phase5]] aren't there because the
namespace stuff is cool. They're there because once the model learns
to think "I should construct the view I need for this task and
operate inside it," **the agent gets capabilities no Linux agent can
match.** Not faster — *different*. Composable in ways global-mount-
table operating systems make hostile.

## The "30 years ahead" thing, said precisely

People say Plan 9 was ahead of its time. The honest version is more
specific: **Plan 9 designed its OS around an agent-loop-shaped
problem 25 years before anyone needed agent loops**. Rob Pike wanted
ordinary humans to compose distributed services from a uniform
interface. That happens to be exactly what an LLM needs to do
effectively. The unit of operation in both cases is "read a file, do
something, write a file" — and "the file" might live on this machine,
that machine, a synthesized server, or a window on the screen.

Pi9 isn't ported FROM Linux TO Plan 9 with the namespace tools bolted
on. It's discovering that the OS already had the abstractions ready
for it. **The introspection is just the first proof.**

## What's still hypothetical

To be honest about what's wired vs what's potential:

- ✅ `ns`, `walk`, `hget` verified in pi9 [[pi9-phase5]]
- ✅ `bind`, `mount` syscall wrappers exist, not yet visually exercised
- ⏳ Skill-scoped namespaces — design idea, not built
- ⏳ Per-task `rfork(RFNAMEG)` sandboxing — design idea, not built
- ⏳ Reproducible namespace replay — `ns` output is already replayable,
  just needs a `replay_ns` tool that reads a dump and re-runs each line
- ⏳ Multi-agent shared 9P workspace — pure design, no implementation

The capabilities described above are downstream of Plan 9's
primitives + the tools pi9 already has. Implementing them is mostly
prompt-engineering and small wrapper tools, not new systems work.

## See Also

- [[pi9-architecture]] — overall pi9 design
- [[pi9-phase5]] — when the namespace tools landed
- [[pi9-for-porting]] — honest assessment: do these tools help when
  porting unix software to plan9? (companion to this page)
- [[vt-9p-namespace]] — example of a per-process 9P file system pi9 uses
- [[mxio-design]] — another 9P file server in the project
- `intro(4)` and `bind(1)` man pages on a live 9front system
- Rob Pike, "The Use of Name Spaces in Plan 9" (1992) —
  the original paper that designed this
