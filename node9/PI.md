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
- **The interactive TUI.** pi selects print mode when stdin/stdout are not a TTY, which is
  what happens here. `process.stdin.setRawMode` exists (it writes `/dev/consctl`), but the
  TUI also needs `isTTY`, terminal size, and key decoding wired up.
- **OAuth login flows** that need a local HTTPS redirect server (`https.createServer`
  throws) or JWT service-account signing (no asymmetric crypto). API-key auth works.
- **Image/clipboard paths** that use the WebAssembly photon codec.
- **`--mode rpc`** and any pi extension whose entry module uses top-level `await`: see the
  `js_std_await` note in DOCUMENTATION.md (fixed in `n9_cli.c`, pending a `qjs` rebuild).
