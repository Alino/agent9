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

pi is 140 packages / ~19k files. `npm install` *works* on node9, but arborist's dependency
resolution for a tree that size is impractically slow on an interpreter, and npm's JS tar
extraction costs ~0.3 s per file. Resolve on the host, unpack on the box with the native
tools:

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

- **The `bash` tool.** pi shells out to `bash`; 9front has none, and node9's
  `child_process` runs commands synchronously without streaming stdout/stderr. Making pi's
  tools work needs a real `child_process` (rfork + exec + pipes) and a shell — this is the
  next milestone, and it is why `src/pi9` uses `run_rc` instead.
- **The interactive TUI.** pi selects print mode when stdin/stdout are not a TTY, which is
  what happens here. `process.stdin.setRawMode` exists (it writes `/dev/consctl`), but the
  TUI also needs `isTTY`, terminal size, and key decoding wired up.
- **OAuth login flows** that need a local HTTPS redirect server (`https.createServer`
  throws) or JWT service-account signing (no asymmetric crypto). API-key auth works.
- **Image/clipboard paths** that use the WebAssembly photon codec.
- **`--mode rpc`** and any pi extension whose entry module uses top-level `await`: see the
  `js_std_await` note in DOCUMENTATION.md (fixed in `n9_cli.c`, pending a `qjs` rebuild).
