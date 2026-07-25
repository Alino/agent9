# Running the real `pi` coding agent on 9front

Upstream **pi** (`@earendil-works/pi-coding-agent`, pi.dev) — the actual published npm
package, unmodified — installs and runs on 9front through node9. This is the TypeScript
agent that `src/pi9` mirrors, running on the real thing rather than a reimplementation.

Verified on bare metal (`cirno`, 9front amd64) with pi **0.82.0**:

```
term% pi --version
0.82.0
term% pi --help
pi - AI coding assistant with read, bash, edit, write tools
...
term% pi --provider anthropic --model claude-haiku-4-5 --api-key sk-... -p 'say hi'
```

The last command performs a real turn: it builds the request, opens TLS to
`api.anthropic.com` over `/net`, streams the response, and prints the reply (with a bad key
you get the API's own `401 invalid x-api-key`, which is the same path proving out).

## Install

pi is 140 packages / ~19k files. npm itself runs fine on node9 (`npm install left-pad`
finishes in 3 s), but a tree this size is a different animal: arborist's resolution alone
burned 13 minutes of CPU without producing a `node_modules/` entry, and npm's JS tar
extraction costs ~0.3 s per file on top. Resolve on the host, unpack on the box with the
native tools — `hget` + `gunzip` + `tar` are C, and the whole tree lands in ~2 minutes:

```sh
# 1. host: resolve the tree with a real npm, then flatten the lockfile to a manifest
mkdir /tmp/pi && cd /tmp/pi && echo '{"name":"x","private":true}' > package.json
npm install --ignore-scripts --omit=optional @earendil-works/pi-coding-agent
python3 node9/tools/lock2manifest.py package-lock.json > pi-manifest.txt

# 2. serve the manifest + installer to the box (the usual agent9 loop)
python3 -m http.server 8765
```

```rc
# 3. on 9front: fetch and install (hget + gunzip + tar, all native C)
hget http://<host>:8765/n9inst.rc >/tmp/n9inst.rc
hget http://<host>:8765/pi-manifest.txt >/tmp/pi-manifest.txt
chmod +x /tmp/n9inst.rc
/tmp/n9inst.rc /tmp/pi-manifest.txt /usr/glenda/pi     # ~2 minutes for 129 packages

# 4. a wrapper, since Plan 9 has no symlinks and shebangs point at "node"
cat >/amd64/bin/pi <<'EOF'
#!/bin/rc
exec /amd64/bin/qjs /usr/glenda/pi/node_modules/@earendil-works/pi-coding-agent/dist/cli.js $*
EOF
chmod +x /amd64/bin/pi
```

`n9inst.rc` is re-runnable and replaces package directories rather than merging them. It
installs from the same registry tarballs npm would use; integrity is bounded by the
manifest's URLs (add SRI checking there if you need it).

## Startup takes about 8 seconds

`pi --version` measures ~7.8s wall / 4.8s CPU on cirno, against 0.12s for bare `qjs`: that
is node9 compiling pi's module graph — 1084 modules — on every run. It is a wait, not a hang.

**A compiled-module cache was built and rejected on measurements** (`JS_WriteObject` at
compile time, `JS_ReadObject` on later runs, entries validated against the source's size and
mtime, in `n9_module_loader`). Two independent reasons:

- **It was slower.** Caching all 1084 modules took startup from 7.6s to 10.5s. Decoding
  bytecode is only marginally cheaper than parsing source in this engine, and the per-module
  overhead (stat the source, stat and read the entry) more than eats the difference. Raw I/O
  is not the problem: reading the whole 8 MB cache with `cat` costs 0.19s.
- **The reader faults on large modules.** With a 32 KB threshold (cache only the files where
  decoding could pay) the second run died in `JS_ReadObject`:
  `suicide: sys: trap: fault read addr=0x31600000004`. That address shape is the kencc
  pointer-math family this port has hit before (see `port/plan9/NOTES.md`) — a 32-bit index
  scaled into a pointer — here inside the bytecode reader's table walk.

So the cost stands until someone fixes the reader (a disassembly hunt like the arg-hoist bug),
and even then the payoff looks small unless the per-module overhead goes away too — one
bundled snapshot per entry point rather than 1084 files. Caching parsed `package.json` files
(kept) did not measurably help either.

## Pointing it at a local model

Any OpenAI-compatible server works — declare it in `~/.pi/agent/models.json` on the box
(this is pi's own config file, no patching):

```json
{
  "providers": {
    "optiq-local": {
      "name": "OptiQ local",
      "baseUrl": "http://192.168.88.10:11435/v1",
      "api": "openai-completions",
      "apiKey": "sk-optiq-local",
      "models": [
        { "id": "default", "name": "Qwen3.6-35B-A3B-OptiQ-4bit",
          "reasoning": false, "input": ["text"],
          "cost": { "input": 0, "output": 0, "cacheRead": 0, "cacheWrite": 0 },
          "contextWindow": 32768, "maxTokens": 4096 }
      ]
    }
  }
}
```

```
term% pi --list-models
provider     model    context  max-out  thinking  images
optiq-local  default  32.8K    4.1K     no        no

term% pi --provider optiq-local --model default -p 'What is 2+2? Reply with just the number.'
4
```

Validated against a Qwen3.6-35B-A3B (MLX, 4-bit) server on the LAN: plain-HTTP transport,
streamed SSE deltas, multi-sentence generations, and **tool calls** — pi's `read` tool
returned a token written to the box a second earlier, and its `write` tool created a file
on hjfs with the exact requested contents.

## Interactive mode

`pi` with no `-p` runs its full TUI on 9front: status bar, footer, editor, spinner, streamed
assistant output, tool-execution blocks, token/context stats. Multi-turn works (ask a
follow-up and the model still has the conversation), tool calls render (`read
~/pitools/secret.txt`), and a reply lands **1–3 s** after you hit enter against the local
model.

### Where to run it

| Terminal | Works | Notes |
|---|---|---|
| **alacritty9** (inside rio) | yes | best experience: size is published and tracked on resize |
| **drawterm -G** (console) | yes | set `LINES`/`COLS` yourself: `LINES=40 COLS=120 pi` |
| bare **rio window** | no | rio's cons is not an ANSI terminal: it does not act on cursor moves, so a redraw appears as duplicated lines. Run alacritty9 (or `vt`) inside it. |

It needs the terminal to say how big it is. Under **alacritty9** that is automatic: Plan 9
has no pty, so it runs its child on plain pipes and publishes the window size in
`/env/LINES` and `/env/COLS`, which is what node9's TTY detection keys off (see
DOCUMENTATION.md). A console session publishes no size at all — and there is nothing to
query, because drawterm's input path swallows a terminal's cursor-position reply (measured:
the query reaches the far terminal, the answer never comes back) — so set the two variables,
exactly as neovim9 does. Without them you get 80x24, which is why pi's box looks narrow in a
larger window. `NODE9_TTY=1/0` forces terminal-ness either way, and `NODE9_TTY_DEBUG=1`
writes what was detected to `/tmp/n9tty`.

**Keys on a console:** Enter works — a Plan 9 keyboard sends `\n` for Return and every
terminal app expects `\r`, so node9 translates it while it holds the console in raw mode.
`ctrl+d` exits pi; `ctrl+c` *clears the input* (that is pi's own binding, see its footer) and
does not quit. To kill a wedged pi from another window: `kill pi` (or
`echo kill >/proc/<pid>/note`).

Getting here took three event-loop fixes in the engine (all in `port/plan9/patch.sh`, so a
rebuild keeps them). The decisive one is an upstream quickjs-libc bug:
`js_os_poll_internal()` overwrites its `nfds` with `poll()`'s return value — the *number* of
ready fds — and then scans only that many leading entries of the array. `poll()` does not
compact the array, so a ready fd at a later index is never dispatched. With stdin registered
first and the response socket third, every streamed reply sat unread until stdin happened to
go ready too: measured **80 s** of stall for a response the server had delivered in 0.6 s.
The other two: an expired timer used to return before any fd was polled at all, and APE's
`select()` reports nothing on a zero timeout (poll.h widens it to 1 ms).

## What pi needed from node9

Each of these is now in `lib/boot.js` and covered by a test in `examples/`:

| Gap | Fix |
|---|---|
| `fetch`/`Headers`/`Response`/`ReadableStream` absent (pi's provider layer is 72 `fetch` calls + SSE) | Web layer over node9's `http`/`https` (`fetchtest.js`) |
| `undici` imported at startup; its HTTP parser is WebAssembly | builtin dispatcher-shaped `undici` shim, `install()` is a no-op |
| `import` of a CommonJS dependency failed | Node's ESM/CJS rules + a generated re-export bridge (`esm-interop-test.mjs`) |
| `"./providers/*"` wildcard `exports` unresolved | pattern matching in the ESM resolver |
| bare `os` resolved to QuickJS's engine module, not `node:os` | `os` is `node:os`; the engine's is `qjs:os` |
| `console.error` missing — every library error path threw instead of reporting | full `console` family (`runtime-test.js`) |
| `Intl` absent (pi-tui measures text with `Intl.Segmenter`) | `Segmenter`/`Collator`/`NumberFormat`/`DateTimeFormat` |
| `process.stdin` was a stub that never emitted | real `Readable` over fd 0 (`stdin-test.js`) |
| `fs.watch`/`watchFile` missing | polled watchers |
| `FormData`, `AbortSignal.any`, `crypto.getRandomValues` missing | added |

## What does not work yet

- **The `bash` tool** — and only that tool. The file tools (`read`/`write`/`edit`/`grep`/
  `find`/`ls`) are pure JS and work, verified with real tool calls. The shell tool does not:
  `spawnSync("which", ["bash"])` returns 127 (9front has no bash), and node9's
  `child_process` *runs* a command but captures nothing — its output goes to the inherited
  fds, so `stdout`/`stderr` come back empty. A shell-tool turn currently exits 0 with no
  output. Fixing it needs a real `child_process` (rfork + exec + pipes) and an `rc`-shaped
  shell tool — the next milestone, and why `src/pi9` has `run_rc` instead.
- **~~The interactive TUI~~ — works now.** See "Interactive mode" above.
- **OAuth login flows** that need a local HTTPS redirect server (`https.createServer`
  throws) or JWT service-account signing (no asymmetric crypto). API-key auth works.
- **Image/clipboard paths** that use the WebAssembly photon codec.
- **`--mode rpc`** and any pi extension whose entry module uses top-level `await`: see the
  `js_std_await` note in DOCUMENTATION.md (fixed in `n9_cli.c`, pending a `qjs` rebuild).
