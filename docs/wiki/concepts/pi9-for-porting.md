---
title: pi9 for Porting Software to Plan 9
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [plan9, arch, porting]
---

# pi9 for Porting Software to Plan 9

> **TL;DR.** pi9's namespace tools make pi9 a *better* porting
> assistant than a Linux-based agent would be, but not a
> *fundamentally different* kind of one. Namespaces help with
> experiment hygiene and mechanical setup; the conceptual translation
> work (epoll → channels, signal → note, autoconf → mkfile) is
> bounded by model capability and skill quality, not tooling.

The honest framing: namespaces help porting *because* porting
involves a lot of "set up the right environment for this attempt"
— but that's a useful side effect of how Plan 9 works, not the
reason it was designed this way. Don't oversell. Plan 9 wasn't
"designed for porting"; it was designed for composing distributed
services, and the same primitives happen to be useful here.

This page is the honest companion to [[plan9-namespaces-for-agents]]
(the conceptual case for agent-native plan9) and [[llm-porting-workflow]]
(the methodology page). Three sides of the same triangle: theory,
methodology, and "what concretely does pi9 add to porting?"

## Where namespace tools genuinely help

### 1. Sandboxed port attempts

`rfork(RFNAMEG)`, bind only the source tree + `/bin` + a clean build
dir, attempt the build. If it explodes — and it will, many times —
the wreckage is contained. The user's `$home` doesn't accumulate
half-finished port detritus.

There's no "make clean" problem because **the namespace itself is
the cleanup**. Pi9 could try ten different #ifdef approaches in ten
separate namespaces, in parallel, without state collision.

Today's Linux equivalent is containers — but containers are
sysadmin tooling, not something an agent reaches for mid-task.
Plan 9 makes it a one-tool-call operation:

```
sandbox({paths: ["/bin", "/srv", "$home/work/port"], cmd: "mk all"})
```

That tool doesn't exist yet in pi9. Proposed in "Concrete next steps"
below.

### 2. Read-only source + writable patch overlay

This is **uniquely a plan9 idiom** and it's exactly the shape porting
wants.

```
bind -b /n/upstream /n/work        # read-only source
bind -b /tmp/patches /n/work       # writable overlay
cd /n/work
edit foo.c, bar.c, mkfile          # model thinks "in source"
                                   # writes go to overlay
```

Diff `/tmp/patches` against the upstream tree at the end. **That diff
IS the patch.** No `git stash` dance, no accidental commits, no "did
I revert all my exploratory hacks." Linux has overlayfs but it's
clunky to use mid-task. Plan 9 makes it casual.

### 3. Filesystem-level tracing

Instead of strace/dtrace to figure out what unix files a program
tries to open, mount a logging 9P file server at the path the program
expects. Every `open(2)` becomes a log entry.

The model sees exactly which `/etc/*` or `/usr/share/*` paths need
plan9 equivalents. **This is strace via composition** — and it works
on libraries that strace can't easily reach into.

Real plan9 trick: build a tiny ramfs that just logs reads and serves
empty files. Bind it over the path the port expects. Run the binary.
Read the log. Now you know exactly which files it cared about.

### 4. Cross-host source access

"Port nginx from the Linux box where I have the latest version."

```
srvssh linux-box
mount /srv/linux /n/linux
walk /n/linux/home/alex/src/nginx
```

The model operates on the source where it lives. No `scp` dance, no
"which version did I copy?" tracking. Particularly useful for
porting a moving target (latest upstream, security patches).

### 5. APE namespace stitching

Plan 9's ANSI/POSIX Environment (APE) provides unix-ish headers +
libc, but it's **not in the default namespace**. A port that needs
`<signal.h>` will fail until `/$cputype/include/ape` is bound onto
`/$cputype/include`.

Today people do this with `eval ape/psh` wrappers (which set up an
APE-flavored shell). Pi9 would do it as a tool call:

```
bind({src: "/$cputype/include/ape", dst: "/$cputype/include", flag: "before"})
```

Surgical, per-port, doesn't affect the rest of the system or the
user's shell. The model can also `bind` it OFF mid-port if the unix
header is causing confusion ("oh, this is plan9 signal-as-note
territory now, kill the APE bind and use `<u.h>`").

### 6. Hermetic build verification

Once a port "works," verify it really does by running the test suite
in a fully-isolated namespace: no PATH inheritance, no env vars from
the porting session, no accidental dependencies on /tmp files left
over from experiments.

```
rfork RFNAMEG
bind /port/result /n/test
bind /bin /bin
cd /n/test
mk test
```

If this works, the port is real. If it doesn't, you had hidden
dependencies. Catches a class of "works on my machine"
fake-completions that's hard to surface on Linux without a CI run.

## Where namespace tools DON'T help at all

The actually hard part of porting is **conceptual mapping**. No
amount of `mount` and `bind` translates these:

| Unix idiom | Plan 9 equivalent | Translation difficulty |
|---|---|---|
| epoll / select | libthread channels + alt() | Hard — different concurrency model |
| pthreads | rfork procs + qlocks | Hard — different memory model |
| signal handlers | notes (note(2)) | Hard — can't longjmp out of notes |
| /proc/N/maps | /proc/N/segment + /proc/N/text | Medium — different layout |
| getaddrinfo | write to /net/cs, read result | Easy once you've done it once |
| TLS via openssl | factotum + /sys/lib/tls | Hard — different auth model |
| socket()/bind()/listen() | write to /net/tcp/clone | Medium |
| select(2) variants | alt() (different shape, similar idea) | Medium |
| dlopen/dlsym | doesn't exist; recompile | Architectural change, not a syscall map |

This is **reasoning work**, not namespace work. Reading 4000 lines
of C and understanding what the I/O layer is *trying to accomplish*
is bounded by model capability and skill quality, not by tooling.

The wiki page [[llm-porting-workflow]] is about exactly this work.
Namespace tools sit beside those skills, not inside them.

## Library availability is also flat

Pi9 mounting things doesn't conjure ported libraries into existence.
If the thing you're porting needs libssl, libuv, libcurl, someone
still has to port those first. The dependency graph is the problem;
namespaces don't shrink it.

What they CAN help with: identifying what's missing. `mk` fails to
link → model walks `/sys/lib/` looking for what's there →
cross-references against the port's needed libs → reports "we need
libev or libuv ported first." That's diagnostic help, not bridging
help.

## Honest size of the win

For an experienced ports developer with their dotfiles ready, the
namespace tools shave maybe 30% off iteration time on the
experiment-heavy parts. The conceptual heavy lifting is unchanged.

For pi9 as an agent — where each "make a small change, build, see
what failed" cycle is gated by the model's reasoning speed — the
isolation properties matter more. **Confidence to try riskier
experiments** because rollback is automatic. That's a quality
multiplier on porting attempts, not a speed multiplier on each
attempt.

Real test would be: hand pi9 a small unix utility (think `jq` or
`fzf`, ~5K LOC C, minimal external deps) and ask it to port. See how
far it gets with namespace-shaped experimentation vs without. Until
we run that test, "namespace tools help porting" stays a hypothesis.

## Concrete next steps

Not as part of pi9 core. As a **skill + 3 helper tools**.

### Tools

1. **`sandbox(paths_visible: []string, cmd: string)`**
   Wraps `rfork(RFNAMEG)` + a sequence of `bind`s + `exec(cmd)` into
   one call. Captures stdout/stderr/exit. Namespace cleaned up when
   the spawned cmd exits.

   This is the killer experiment-running primitive. Model says
   "try the build in a sandbox seeing only these paths." Failure is
   contained, retries are cheap.

2. **`overlay_diff(base: string, work: string)`**
   Returns a unified diff between the read-only base and the writable
   overlay. After many experimental edits, this surfaces the actual
   patch in shippable form.

3. **`srvssh_mount(host: string, mountpoint: string)`**
   `srvssh host` + `mount /srv/host mountpoint` in one call. Common
   pattern for cross-host work; worth a dedicated tool because the
   two-step form is hard for the model to remember reliably.

### Skill

`port-c-to-plan9.md` capturing the conceptual map:

- The translation table above (epoll → alt, signal → note, etc.)
- APE binding ritual at start of every port
- The overlay pattern (when to use it, when not)
- Common gotchas: rune vs char, no `void *`, no varargs (well, kinda)
- Build system: mkfile structure, $cputype, /$O/etc
- When to give up: heuristics for "this dep tree is too deep, fail
  loud rather than ship a fragile port"

About 2-3KB markdown. Loaded on demand via `read_skill("port-c-to-plan9")`.

### Test

Hand pi9 a small C utility from github (not too dependent, not too
unix-specific):

- Easy: `xxd` clone, ~500 LOC, just I/O — should be a half-hour port
- Medium: `jq` core (no JSON-C dep), ~3K LOC — full-day project
- Hard: a small TLS-using thing — exercises factotum, hard part

Measure: how many iterations, where does pi9 get stuck, what skills
need refinement. Update the skill, retry. Same pattern as how Claude
Code skills evolve.

## What I'd push back on if someone tried to oversell this

- **"Plan 9 makes porting easy."** No. It makes the porting workflow
  cleaner. The conceptual work — understanding what code is trying
  to do and rewriting it against different primitives — is unchanged.
  Anyone who's actually ported software to plan9 (e.g. the 9front
  developers porting Go, Mercurial, Mosh) will tell you it's hard.

- **"pi9 will port everything for me."** No. Pi9 will be a better
  porting *assistant* — it can iterate experiments cheaply and keep
  the workspace clean. Architectural decisions (which design to keep,
  which to discard, where to use rc vs Go, whether the port even
  makes sense) need a human or a much stronger model.

- **"Namespaces > containers."** Not the right comparison.
  Containers are about isolation for production; namespaces are about
  composition for development. They solve different problems. Plan 9
  doesn't have a containers equivalent, and Linux containers don't
  have a namespace equivalent. Neither is strictly better.

- **"Plan 9 was designed for porting."** Nope. Plan 9 was designed
  for composing distributed services from a uniform interface. The
  fact that this happens to be useful for porting is a side effect
  of good primitives, not the intent.

The honest claim: **pi9 + Plan 9 namespaces should make small-to-
medium port attempts (a few thousand LOC, minimal deps) noticeably
faster and cleaner than the same work on Linux**. Large ports (a
million LOC, deep dep graph) are still hard; namespace tools save
maybe a day on the experiment-heavy parts, but don't shorten the
months-of-deep-work parts.

## Status

Speculative. None of the proposed tools or skills exist yet. Pi9
Phase 5 wired the building blocks (`bind`, `mount`, `walk`, `ns`).
Phase 6 will be polish, Phase 7 xena-panel integration. Porting
helpers would land as a separate workstream — let's call it Phase
"port" — without disrupting the chat-agent core.

If we ever run the small-utility port test described above, this
page gets revisited with real numbers. Until then: hypothesis.

## See Also

- [[plan9-namespaces-for-agents]] — why per-process namespaces
  matter for LLM agents generally
- [[llm-porting-workflow]] — the methodology page (port the design,
  not the API; redesign beats translate; three porting modes)
- [[pi9-architecture]] — pi9's overall design
- [[pi9-phase5]] — where bind/mount/walk/ns were wired up
- [[build-toolchain]] — practical cross-compile mechanics from
  Mac → 9front
- `intro(1)`, `bind(1)`, `rfork(2)` man pages on a live 9front
  system — the primary sources
