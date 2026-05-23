---
title: LLM-Driven Porting Workflow — Linux/BSD/Win C → Plan 9 / 9front
created: 2026-05-16
updated: 2026-05-16
type: concept
tags: [arch, toolchain, decision, cross-compile, status: wip]
sources:
  - raw/articles/talos-llm-reverse-engineering-sidekick-2025-07.md
  - raw/articles/project-naptime-google-zero-2024-06.md
  - raw/articles/team-atlanta-aixcc-final-2025-08.md
  - raw/articles/llm4decompile-github-2026-05.md
  - raw/articles/claude-share-llm-re-porting-asahi-2026-05.md
confidence: medium
---

# LLM-Driven Porting Workflow — to Plan 9 / 9front

How to use the [[llm-reverse-engineering]] toolchain to port C software to
Plan 9. Targets Hermes `/goal` as the orchestrator. Confidence is medium —
this is the **proposed** pipeline; specifics will harden as we run it on
real ports.

---

## Why /goal is the right harness

Hermes `/goal` ships these properties (matching Naptime's five principles):

| Naptime principle | /goal feature |
|---|---|
| Space for reasoning | Default chain-of-thought, no straitjacket |
| Interactive environment | Full Hermes harness — terminal, file, web, MCP |
| Specialised tools | Skills + MCP tools (incl. GhidraMCP, ida-pro-mcp) |
| Perfect verification | The **judge** model — sees goal + last 4KB output + system prompt; rules done/not-done |
| Multi-trajectory sampling | Survives crashes/restarts, can be resumed, sub-goals can be added mid-run |

The judge is the killer feature. Without it you have a Ralph-loop (infinite
file-write loop) that burns tokens forever. With it you have a closed-loop
agent that stops when the test oracle says "done".

---

## The Plan 9 mindset — port the design, not the API

**The biggest pitfall is treating porting as 1:1 API translation.** Plan 9
isn't "Unix with different syscalls" — it's a different design built on
two ideas that Unix only half-committed to:

1. **Everything is a file, including services.** The right Plan 9 expression
   of "expose state X" is almost always a 9P fileserver under
   `/n/<service>`, not a POSIX socket, not a signal, not shared memory.
2. **Composition over abstraction.** Small programs, separate processes
   joined by pipes/9P/channels, no in-process plugin systems.

So before reaching for any POSIX→Plan 9 mapping, ask in order:

| Question | If yes |
|---|---|
| Does Plan 9 already meet this need a totally different way? | **Don't port. Use what's there.** |
| Is the POSIX-ism load-bearing for the user-visible function, or just for the implementation? | If just implementation: **delete it, redesign the implementation.** |
| Would a Plan 9 person have ever written this code in this shape? | If no: **redesign, not translate.** |
| Is this an actual user feature that has no Plan 9 equivalent? | Only now: translate via the mapping table below. |

### Examples — when redesign beats translation

| POSIX-shaped thing | Plan 9-native replacement |
|---|---|
| Daemon listens on TCP `:5555` for control commands | 9P fileserver mounted at `/n/svc/foo`. Clients `echo cmd > /n/svc/foo/ctl`. No socket protocol to invent. |
| Program reads/writes `~/.config/foo.json` | Bind a per-user namespace and let the data live in a file; or expose state as files under `/n/foo/`. |
| Tail a logfile + parse for events | Tail an event stream from a 9P fs the producer exposes. `read` blocks naturally. |
| inotify / fsevents on a directory | Have the producer write `qid.vers` events to a 9P fs you mount. |
| Status / metrics over `/metrics` HTTP | Expose under `/dev/<myservice>/stats` — just files. Tools like `cat`, `grep`, `awk` work for free. |
| Pthread thread-per-connection server | `fork`-per-connection with channels for shared state, or one process and `listen` + `accept` loop. |
| Curses TUI with mouse/keyboard | Either: don't have a TUI (Plan 9 culture prefers line-based + acme/win); or use libdraw and write a real window. |
| Locale + i18n config files | Plan 9 doesn't really do locales. Pick UTF-8 (Plan 9 invented it) and stop. |
| Cron jobs via `crontab -e` | `/cron/<user>` files in cron's namespace. Already exists, don't reinvent. |
| Plugins loaded via `dlopen` | Separate processes joined by 9P. No JIT-loadable code in-process. |
| Shared memory via `shm_open` + `mmap` | One process owns the data, others access via 9P reads on a synthetic fs it serves. Or `segattach` if you really must. |
| Bash scripts wrapping the program | rc scripts. `rc` is the Plan 9 shell; bash is not coming. |

### When the right port is "no"

Some software shouldn't be ported. Signs:

- The whole value-add is integration with a Linux-only stack (systemd
  units, Wayland, eBPF, cgroups, X11 extensions).
- It exists to paper over POSIX limitations Plan 9 doesn't have (most
  service supervisors, most config-management tools, most "POSIX
  abstraction layer" libraries).
- It re-implements a Plan 9 native facility badly (most `find`
  variants vs `walk(1)`; most config-file daemons vs namespace binds;
  most TUI builders vs acme).

Better Plan-9-native equivalent already exists for: `make` (use `mk`),
`grep`/`find`/`xargs` (Plan 9 has these, sometimes different flags),
TUI frameworks (use acme or libdraw), service supervisors (rfork +
plumber), config-DB tools (namespaces), tmux (use the window system
+ rc).

If the answer is "no, don't port" — say so, don't grind a /goal loop
into a Plan-9-shaped pile of POSIX residue.

---

## Two distinct porting modes

### Mode A — Source-available port (the common case)

You have the C source. The interesting axis is **what to redesign and
what to translate**. The mapping table below is the last-resort tool
for things that genuinely don't have a Plan 9 redesign — apply the
mindset checklist above first.

POSIX→Plan 9 quick reference (only after the redesign questions above):

| Posix idiom | Plan 9 idiom |
|---|---|
| `fork()` | `rfork(RFPROC\|RFFDG\|RFMEM)` |
| `pthread_*` | `threads(2)` + `chan` + `alt()` — and consider whether you need threads at all rather than separate processes |
| `epoll / kqueue / signalfd` | 9P fileserver + `read()` blocks naturally — usually means the whole event-loop architecture goes away |
| `socket` (BSD) | `dial(2)` / `/net/tcp!host!port` — and ask whether the protocol should be 9P instead of TCP |
| `mmap` | `segattach(2)` — but consider serving the data over 9P first |
| `signal()` | `notify(2)` / `atnotify(2)` — but most signal use cases disappear under "small processes + channels" |
| `inotify` | Producer exposes 9P fs; consumer polls `qid.vers` or blocks on read |
| `malloc / free` | `malloc` / `free`, but prefer `mallocz` (zero on alloc) and `sysfatal` on OOM |
| `errno + strerror` | `errstr()` — and `print("%r")` for the formatted version |
| `getenv("HOME")` etc. | `/env/home` and friends; envvars are files too |
| `pipe(2)` (for IPC inside one program) | usually unchanged, but rfork's shared namespace + `pipe` covers more |
| X11 / framebuffer | libdraw on `/dev/draw` ([[draw-api]]) |
| GNU make | `mk` + `Mkfile` |
| bash scripts | `rc` scripts, [[build-toolchain]] for the quoting/redirection traps |

### Mode B — Binary-only port (rare for us, common in malware/RE)

You have an x86_64 ELF or Windows PE and no source. Use LLM4Decompile-Ref
+ Ghidra to recover C, then jump to Mode A. Plan 9 binaries we'd reverse
are essentially never this case — Plan 9 source is public, the interesting
software all has source. Mode B mainly matters for porting legacy DOS
games or proprietary firmware.

### Mode C — Spiritual / behavioural reimplementation (OpenGothic-style)

The interesting middle ground. You have a closed-source binary plus an
existing open-source partial reimplementation (OpenGothic, OpenJK,
OpenMW, OpenDune, Asahi's Mesa driver). Goal is NOT to translate the
binary into source — it's to **port specific behaviours** from the
closed binary into the open codebase, function by function, until
behavioural parity is reached.

Why this is the sweet spot for our /goal pipeline:

- **Specific verifiable queries** instead of blind "find collision code":
  "find the function responsible for NPC daily routine scheduling",
  "find how the original calculates fall damage with Gothic's rounding",
  "find the dialogue choice selection logic". These are testable.
- **Existing reimpl is the oracle**: run both, compare game state /
  output / replay log. Differential test reveals exactly which functions
  diverge.
- **Existing wiki seed**: the OSS project usually already has notes on
  engine internals (ZenGin for Gothic, Mesa for Asahi). Ingest as raw/
  to seed the LLM Wiki.
- **No legal grey area for fully-original spiritual reimpls**, though
  see disclaimer at end of section.

Pipeline (matches the AsahiLinux precedent):

```
        ┌──────────────────────────────┐
        │  Original closed binary      │
        │  (Gothic.exe, AppleGFX, ...) │
        └──────┬───────────────────────┘
               ↓
        ┌──────────────────────────────┐
        │  Ghidra/IDA decompile all    │   one-time, expensive
        │  → summarise each function   │
        │  → embed into vector DB      │
        │  → call graph into graph DB  │
        └──────┬───────────────────────┘
               ↓
        ┌──────────────────────────────┐
        │  Wiki seed: existing OSS     │
        │  RE notes ingested as raw/   │
        └──────┬───────────────────────┘
               ↓
   ┌───────────────────────────────────┐
   │  /goal: port feature X to <OSS>   │
   └───────────────────────────────────┘
               ↓
   ┌───────────────────────────────────┐         ┌───────────────┐
   │ EXECUTOR                          │         │ JUDGE         │
   │  - vector search wiki/index for X │  ←────► │  done when    │
   │  - if absent: vector search       │         │  differential │
   │    pseudocode DB                  │         │  passes       │
   │  - read top-K candidate funcs     │         │  on test      │
   │  - propose <OSS> patch            │         │  scenarios    │
   │  - file new wiki page             │         │               │
   └────────────┬──────────────────────┘         └───────────────┘
                ↓
   ┌───────────────────────────────────┐
   │  DIFFERENTIAL ORACLE              │
   │   run scenario in original.exe    │
   │   run scenario in patched <OSS>   │
   │   diff(state, output, replay log) │
   └───────────────────────────────────┘
```

For Plan 9 specifically: this is how we'd attack a port like "implement
a Plan-9-native version of <some Linux daemon> while staying behaviourally
compatible". The Linux daemon is the oracle; the differential test is
a protocol replay through 9P or a TCP listener.


---

## Proposed /goal pipeline (Mode A)

```
                 ┌────────────────────────────────┐
                 │   /goal  prompt:                │
                 │   "Port <project> to 9front     │
                 │    such that `mk` builds        │
                 │    AND tests/run.rc passes."    │
                 └──────────────┬─────────────────┘
                                ↓
        ┌───────────────────────────────────────────┐
        │  EXECUTOR (Hermes agent w/ Claude/GPT)    │
        │   - clones source                          │
        │   - inspects Makefile / autoconf           │
        │   - writes Mkfile                          │
        │   - rewrites POSIX→Plan9 calls function    │
        │     by function (load                     │
        │     `concepts/llm-reverse-engineering`    │
        │     and this page as skills)              │
        │   - delegates discrete subtasks to        │
        │     Codex CLI sub-agents (per video)      │
        └──────────────┬────────────────────────────┘
                       ↓                       ↑
        ┌──────────────────────────┐  ┌─────────────────┐
        │   ORACLES (test harness) │  │  JUDGE          │
        │   1. cross-compile via   │  │  (separate LLM, │
        │      plan9port `9c/9l`   │  │  sees goal +    │
        │      [[build-toolchain]] │  │  last 4KB +     │
        │   2. push to 9front VM   │  │  system prompt) │
        │      via hget HTTP       │  │  rules:         │
        │      bridge              │  │   - on track?   │
        │   3. run unit tests in   │  │   - done yet?   │
        │      VM                  │  │   - stuck?      │
        │   4. for UI: screendump  │  │                 │
        │      + vision_analyze    │  │                 │
        │      [[testing-harness]] │  │                 │
        └──────────────────────────┘  └─────────────────┘
```

The judge is the difference between a 17-minute /goal that finishes and a
14-hour Ralph loop that didn't.

---

## Concrete starting prompt template

```
You are porting <PROJECT_NAME> to 9front amd64.

Repo: <git URL or local path>
Target: Plan 9 / 9front, C, K&R style, 8-space tabs, libdraw for any UI.

MINDSET (read concepts/llm-porting-workflow.md FIRST):
- Port the design, not the API. Plan 9 is not Unix-with-different-syscalls.
- Before translating ANY POSIX-ism, ask:
  1. Does Plan 9 already meet this need a totally different way?
  2. Is this POSIX-ism load-bearing for user-visible function, or just for
     the implementation? If just implementation: redesign, don't translate.
  3. Would a Plan 9 person have written this code in this shape?
- 9P fileserver beats sockets/signals/shared memory for service exposure.
- Separate processes + channels beat threads + plugins.
- If the right answer is "don't port this, use the Plan 9 native facility",
  SAY SO and stop. A correctly-not-ported program is a success.

Hard rules (from AGENTS.md):
- No Rust, no Python in the runtime.
- No epoll/pthreads/signalfd/etc. Use rfork, threads(2), channels, alt(), 9P.
- mk, not make. Mkfile per component.
- rc scripts, not bash.

Workflow:
1. Read README + every Makefile. Inventory what the program ACTUALLY does
   for the user (separated from how it does it on POSIX).
2. For each subsystem: redesign decision (Plan 9-native | translate |
   delete | refuse). Write the decision to wiki/concepts/<project>-port.md.
3. Only AFTER decisions are written, start implementing. New design first,
   POSIX-to-Plan-9 fallback table as last resort.
4. Generate Mkfile that produces a working binary using 9c + 9l on the
   plan9port toolchain.
5. Build on plan9port (macOS). Fix until clean.
6. Transfer via HTTP/hget to the 9front VM. Verify it runs there too.
7. Run tests/ if present, or hand-craft regression in rc.

Goal criteria:
- `mk` exits 0 in plan9port environment
- binary runs on 9front VM without panic
- ALL behaviour the user actually cares about works (not necessarily 1:1
  with the original — Plan 9 redesigns are encouraged)
- if tests exist: all pass
- if UI: screendump + vision_analyze confirms a non-empty window

Sub-goals you may add mid-run:
- "after each subsystem redesign decision, write a one-line note to log.md"
- "any API I had to invent goes into concepts/posix-to-9p-mappings.md"
- "if you find yourself writing more than 100 lines to emulate one POSIX
   call, STOP and reconsider the redesign — that's a smell"

NEVER edit raw/. NEVER push to GitHub without my approval.
```

---

## Tool wiring (where to install what)

| Component | Where | Notes |
|---|---|---|
| Ghidra 11.x + GhidraMCP | macOS host | Decompile reference x86_64 Linux/BSD binaries when source ambiguous |
| Plan9port (`9c`, `9l`, `mk`) | macOS host | Build oracle for plan9port-buildable subset |
| 9front VM | `~/Projects/plan9-agent/` | Runtime oracle. Boot via `boot-9front.sh`. SSH over aux/listen1 + nc :1717 |
| hget HTTP bridge | macOS host serves binaries on `0.0.0.0:8000`; 9front pulls via `hget` | See [[build-toolchain]] for the `--bind 0.0.0.0` silent-fail gotcha |
| screendump + vision_analyze | Hermes built-in tools | QMP-driven, see [[testing-harness]] |
| LLM4Decompile-9B-v2 (optional) | macOS via Ollama/transformers | Only when binary-only port — Mode B |
| r2 + decai | macOS host | Quick triage of small Plan 9 / Linux binaries |

---

## Failure modes specific to Plan 9

These are NOT covered by the general
[[llm-reverse-engineering]] pitfalls list — they're Plan-9 specific:

- **LLMs love `signal()`. Plan 9 doesn't have it.** Every signal handler
  must become `atnotify(2)`. Models will silently leave `signal(SIGINT,
  handler)` in code that "looks fine" until you `mk` it.
- **LLMs love `printf("%s\n", strerror(errno))`.** Replace with
  `print("%r\n")` or `errstr()`. The model won't know unless this page
  is loaded as a skill.
- **`size_t` and `ssize_t`** — non-standard on Plan 9. Use `long`,
  `vlong`, `usize`, `ulong`. Force explicit types.
- **Memory model.** Plan 9 has no PROT_EXEC for non-text segments by
  default — JIT-y code requires `segattach` with explicit perms.
- **rc shell, not bash.** Test scripts must be rc. LLMs will generate
  bash by default. Sentinel pattern: `>[2=1]` for stderr-to-stdout,
  NOT `2>&1`. Documented in [[build-toolchain]].
- **No /tmp by convention** — `/tmp` exists but Plan 9 idiom is
  `/n/<workspace>` or process-private `#s/srvname`.

---

## Concrete first port to attempt

A small standard-C utility with no graphics and minimal POSIX:

- **jq** subset — JSON pretty-printer. ~3000 LoC, only depends on stdio
  and string libs. Easy first /goal target.
- **tree** — directory walker. Single file, easy.
- **sl** — train ASCII animation. Tiny, builds in seconds, good for
  end-to-end test of the pipeline.

Avoid as first targets: anything with ncurses (no ncurses on Plan 9),
anything with threads-per-connection (different model), anything with
glibc-specific extensions (`getline`, `*v_*`).

---

## What this page is NOT trying to do

- Replace the human judgement on whether a port is worth doing.
- Automate ports of OS-level components (kernel modules, init systems).
  Those are Mode B territory and out of scope for /goal.
- Promise that LLM4Decompile will help on Plan 9 binaries. Plan 9 uses
  its own a.out variant; LLM4Decompile was never trained on Plan 9
  binaries. Use Ghidra alone for those.

---

## See also

- [[pi9-for-porting]] — what pi9 specifically adds to this workflow:
  sandbox/overlay/srvssh-mount tools, port-c-to-plan9 skill. The
  concrete-tools companion to this methodology page.
- [[plan9-namespaces-for-agents]] — the conceptual case for why pi9
  is in a different shape than a Linux porting agent
- [[llm-reverse-engineering]] — the underlying tool landscape
- [[build-toolchain]] — cross-compile Mac → 9front; the oracle leg
- [[testing-harness]] — QMP, screendump, vision_analyze; UI oracle leg
- [[browser-webview-plan9]] — a candidate large port that this workflow
  could attack (browser engine + WebView)
- [[9fans-ecosystem]] — what already exists, so we don't reinvent
