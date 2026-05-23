---
title: "Pi9: I Ported a Modern LLM Agent to Plan 9"
subtitle: "Bubble Tea on 9front, OAuth that ends in a mothra paste, and tool calls that touch namespaces."
author: Alex Sadovsky
date: 2026-05-17
---

# Pi9: I Ported a Modern LLM Agent to Plan 9

Most "I ran an LLM on $weird_os" posts are a screenshot of llama.cpp
printing tokens and a victory lap. This isn't that. Pi9 is a real coding
agent. Streaming chat, tool calling, sessions, skills, persistent
memory, OAuth to Anthropic and GitHub Copilot, a model picker, a slash
command surface. And it runs natively inside 9front. Not in a Linux VM
next door, not over a 9P bridge to a Mac. Inside the box, as a Go
binary in the cwfs file tree, launched from a WinXP-Luna start menu I
also wrote, painted into a libdraw window over a terminal server I
also wrote.

If that sentence sounds unhinged, good. It is.

## Why

Pi (https://pi.dev) is the agent shape I want when I'm coding. Modern
TUI, streaming, tool blocks that collapse, a tree view that lets you
back up to before a bad tool call without losing the dead branch. Pi
is TypeScript on Node. Node does not run on Plan 9 and never will.

I also have my own agent9 project. A 9front desktop port that
uses mxio (rio with WinXP Luna titlebars), xena-panel (the taskbar),
vtwin (a libdraw terminal frontend), and vts (a 9P session multiplexer
that owns the cell grid). I've been daily-driving it. The hole in the
desktop was obvious: no agent. Tinyxena, the proof-of-concept I built
earlier, was a one-shot REPL with three tools. Fine for "does Go even
work on Plan 9?" Not fine for actual work.

So I rewrote it. Bubble Tea plus Lipgloss plus Bubbles for the TUI,
the same provider model and skills system Pi has, OAuth flows for
Anthropic Pro and GitHub Copilot subscribers (BYO key still works), a
tree-structured session store, and a tier of plan9-native tools that
no Linux agent can have because Linux doesn't have the primitives.

It's called pi9. Eleven phases over a couple of weeks.

## The first wall: Bubble Tea doesn't build on Plan 9

`charmbracelet/x/term` has a build constraint that explicitly excludes
plan9 from the fallback path. So a clean `GOOS=plan9 GOARCH=amd64 go
build` of any Bubble Tea program dies at link time with `undefined:
state`, `undefined: makeRaw`, and so on.

The fix is small. Sixty lines of `term_plan9.go` that wrap
`golang.org/x/term`, which *does* support plan9. It opens `/dev/cons`
and `/dev/consctl` and twiddles them. A vendored shim got me building
in a day. A PR to charmbracelet is sitting in the queue.

Same issue surfaced in `muesli/cancelreader`. Same shape of fix. After
that, Bubble Tea ran. Phase 1 ended with a "hello pi9" screen
rendering inside a vtwin window on 9front. Cyan header, dashed input
box, status bar at the bottom. Genuinely emotional moment if you've
spent any time trying to convince Plan 9 to do anything written this
century.

## The TUI stack itself runs unmodified

Once x/term was patched, `termenv`, `lipgloss`, `bubbles` (textinput,
viewport, list), and `bubbletea`'s renderer all worked. `termenv`'s
plan9 path correctly reports `Ascii` (no escape detection), but I pin
it to `termenv.WithProfile(termenv.ANSI)` because vts owns the
16-color WinXP Luna palette and I want consistent rendering.

The one quirk: Plan 9 sends Enter as `\n` (LF), not `\r` (CR). Bubble
Tea's input reader expects CR by default. Translation happens at the
vtwin layer. vtwin sees the keyboard rune `'\n'` and forwards `\r` to
vts, which forwards `\r` to pi9, which Bubble Tea recognizes. Same
story for Plan 9's private-use keyboard runes (`Kup` at 0xF00E,
`Kleft` at 0xF011, the rest). vtwin translates them to xterm CSI
sequences (`\x1b[A`, `\x1b[D`) so every ncurses program, including
pi9, gets the input it expects.

## OAuth that ends in a mothra paste

I implemented OAuth for Anthropic Claude Pro/Max and GitHub Copilot
because those are the providers I actually pay for and I'd rather not
manage API keys.

The chain is the standard PKCE flow. The callback story is the
interesting part. On a normal machine, the browser opens the auth
URL, the user clicks "Authorize", and the IDP redirects to
`http://localhost:53692/?code=...` where the agent has a server
bound. On 9front, "the browser" is mothra, which renders HTML from
2002 and barely speaks HTTPS. So the OAuth provider on pi9 starts a
server on `:53692`, prints the URL into the TUI, and tells you:
open this in mothra or your host browser, click through, then
either the auto-callback fires OR you paste the `<code>#<state>`
blob back into pi9.

Both paths work. The host-browser flow goes through QEMU's hostfwd
back to the VM's `:53692`. The manual paste flow handles the case
where you read the URL off the screen and open it on the Mac.

Pi.dev had the manual-paste fallback already. I just hadn't noticed
until Phase 10 session 2, when I burned an evening trying to invent
the right URL shape and the right `client_id` from scratch. Then I
cloned the upstream Pi repo, grepped, and found every constant
sitting in `packages/ai/src/utils/oauth/anthropic.ts`. Including the
magic system prompt `"You are Claude Code, Anthropic's official CLI
for Claude."` that Anthropic requires for OAuth-authenticated
requests.

Lesson: before writing any code for parity work, clone the reference
and grep. I learned it the way you always learn it.

## Tool calling: portable, then plan9-native

Pi9 has eleven tools split across two tiers.

The portable tier is what you'd expect. `read_file`, `write_file`,
`edit_file`, `run_rc`, `list_dir`. Boring. Necessary. They work
because Go's `os` package on plan9 mostly does what you expect.
`os.OpenFile` maps to `open(2)`. `exec.Command` maps to
`rfork(RFPROC|RFFDG|RFNOTEG|RFENVG)` and an `exec(2)` of `/bin/rc`.
`run_rc("ls /bin")` returns the same strings you'd get typing it.

The native tier is where pi9 stops being a port and starts being
plan9-shaped. `plumb`, `hget`, `walk`, `ns`, `bind`, `mount`.

`ns()` dumps the agent's current namespace. The LLM can literally
read the file tree it lives in. Every mount, every bind, every
`/srv/` endpoint, visible as text. I added `ns()` thinking it would
be diagnostic. It's not. The model started using it to *plan*.
"What's the path to TLS roots in this VM?" → `ns({"filter":
"/sys/lib/tls"})` → resolves the path → uses it. I didn't prompt
for that and there's no skill telling it to do it. It just looked.

`plumb(text, port)` sends data through plan9's plumber. Asking pi9
"what's that error mean?" with a URL in scrollback ends with the URL
plumbed to `web`, opened in mothra. Asking it to "open the source
file we just edited" plumbs to `edit`, popping it in acme. Or in my
case, in a new vtwin pane via my custom edit handler.

`bind` and `mount` are syscalls. They're build-tagged behind
`golang.org/x/sys/plan9` because they have no equivalent on
darwin/linux, and the non-plan9 build emits stubs that return "Plan
9 only" errors. The LLM can compose a sandbox by `bind`ing source
plus `/bin` plus a scratch dir into a fresh namespace, run `mk all`
inside it, and return the result. The wreckage of a failed build
attempt stays inside a namespace nobody else can see.

This is the killer feature. You can't get it on Linux without
containers, and containers are sysadmin tooling, not something an
agent reaches for mid-task. On Plan 9 it's a tool call.

## Sessions, skills, memory

Three pieces, lifted in spirit from Claude Code and Pi.

Sessions live as JSON files in `/usr/glenda/lib/pi9/sessions/`. They
are tree-structured. Every message has a `parent_id`, so backtracking
past a bad tool call is a single command. The active session is a
path in `/usr/glenda/lib/pi9/sessions/current`. The agent reads its
own session file when it needs to remember "what was the path you
edited 20 turns ago". Cheaper than re-emitting the whole transcript
through the LLM.

Skills are markdown files with YAML frontmatter in
`/usr/glenda/lib/pi9/skills/`. The descriptions are listed in the
system prompt every turn; the bodies load on demand. Same
progressive-disclosure trick Anthropic ships in Claude Code. My
skill files are symlinks into the project wiki, so authoring a wiki
page IS authoring a skill.

Memory is one file, `/usr/glenda/lib/pi9/memory.md`, loaded every
turn. Soft cap 4KB. The `remember(content)` tool appends to it.
Above 4KB, pi9 nags me to prune or graduate facts to skills.

## The bug that taught me to read source files instead of asking vision

Phase 6 had a stale-frame bug. Bubble Tea's diff renderer was
leaving stripes of the previous frame at the bottom of the viewport
on scrollback updates. I spent an hour squinting at screenshots
through vision, asking "is this the right color in the title bar"
and "does the input box have a leading newline".

Then I remembered that pi9 persists everything. The active session is
a JSON file with every message, every tool call result, every
rendered chunk. So I read the file. Bug visible at line 412: the
viewport's width was computed before `tea.WindowSizeMsg` arrived, so
the initial render used a default 80-column assumption, and the diff
renderer didn't know to invalidate the whole region when the actual
width landed.

Fix: pin to ASCII borders (not box-drawing), insert a
`tea.ClearScreen` after the first real size message, and add
`fitRow`/`fitBlock` helpers that explicitly truncate by ANSI-aware
cell width. The bug was gone the same day.

Vision is for layout. The session JSON is for content. Don't confuse
them.

## Performance is fine

Go on Plan 9 has no JIT, no platform-specific runtime tuning, and
the VM is TCG-emulated amd64. Pi9 still streams tokens in real time
because the workload is network-bound. A typical tool turn:

- 50ms to hit the provider
- 200-800ms for the SSE stream
- 30-50ms to dispatch the tool (walk, hget, ns are all under 100ms)
- under 16ms for the diff renderer to paint the new frame

The 60-second `mk install` for NetSurf on the same VM is more
painful than every pi9 turn ever has been.

## What's left

A header that doesn't scroll off-screen after `tea.ClearScreen`. Two
known fixes, picking one in Phase 12.

Real mouse-wheel testing. QMP can't simulate it cleanly.

A `sandbox` tool that wraps `rfork(RFNAMEG) + bind` into one call.

Actually trying to port a real piece of software with pi9 driving.
Current candidates: `jq` core, `fzf`. The pi9-as-porting-assistant
claim is speculation until that's done.

Stretch: a xena-panel status indicator that shows when pi9 is
thinking or running a tool, so the agent is legible from outside its
window.

## Does this matter

Honestly, no, in the obvious sense. Total user count of pi9 right
now is one and it's me. Nobody else runs Plan 9.

But the experience of an agent that can read its own namespace,
compose a sandbox in a single tool call, and bind a remote host's
source tree into its view to run tests is different from what a
Linux agent does today. Not "slightly faster" different. The shape
of the operation is different. On Linux the agent shells out to
docker or unshare and burns a turn negotiating with the host. On
Plan 9 the agent edits its own view of the filesystem and the
question goes away.

I don't think Plan 9 wins. It's missing a lot, starting with anyone
to maintain it. But it does happen to have the primitives an agent
wants, by accident, twenty years before agents were a thing. Worth
noticing.

Source, wiki, screenshots: https://github.com/Alino/agent9. Eleven phase
write-ups in `wiki/concepts/pi9-phase*.md`. If you're already
convinced Linux's namespace story is too thin for agents, read
`wiki/concepts/plan9-namespaces-for-agents.md` first.

— Alex
