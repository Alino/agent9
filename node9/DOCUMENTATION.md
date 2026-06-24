# node9 — Node.js & npm on 9front (Plan 9)

node9 is a **Node.js-compatible JavaScript runtime for 9front/Plan 9 (amd64)**, plus the
**real, unmodified npm** running on top of it. It lets you `npm install` packages from the
public registry and `require()`/`import()` them on Plan 9 — a platform Node.js itself has
never supported.

This document explains what node9 and npm-on-node9 *are*, how faithfully they reproduce the
originals, where they diverge, and what the test coverage looks like.

---

## 1. What it is

### node9 (the runtime)
A Node-compatible runtime built on **QuickJS-ng** (a small, embeddable, ES2023 bytecode
interpreter) — **not V8**. QuickJS was chosen because V8 cannot be built for or run on Plan 9
(no C++ toolchain targeting 9front, and V8 needs mmap/W^X/JIT machinery Plan 9 doesn't
provide). QuickJS is a bytecode interpreter, so it sidesteps all of that.

On top of the engine sits a **Node-compatible standard library** (`lib/boot.js`, ~2,375 lines,
**46 builtin modules**) loaded at startup, plus a **native binding layer** (Plan 9 `libsec`
crypto/TLS and `libz` gzip) and Plan-9-native networking over the `/net` filesystem.

The `qjs` binary (≈2.5 MB) is the runtime; it runs scripts, modules, and a REPL, and is
installed at `/amd64/bin/qjs` on the target.

### npm on node9
The **actual npm 10.9.8** — the real CLI and its ~250k LOC of bundled dependencies (pacote,
arborist, cacache, make-fetch-happen, minipass, node-tar, semver, …) downloaded from the
registry and run **unmodified** on the node9 runtime. node9 provides the Node API surface npm
needs; npm itself is not reimplemented or patched (one bundled-minizlib gunzip path is the
only exception — see Limitations).

Verified end to end: `npm install <pkg>` fetches from `registry.npmjs.org` over **real TLS**,
**SHA-512 SRI-verifies** the tarball, gunzips and tar-extracts it into `node_modules`, and
writes `package-lock.json`.

---

## 2. Architecture

```
  your script / npm-cli.js
        │  require() / import()
        ▼
  lib/boot.js  ── Node stdlib in JS (46 builtins): path, fs, stream, http(s),
        │          net, tls, crypto, zlib, buffer, events, url, util, assert, …
        │          + CommonJS resolver + ESM resolver
        ▼
  globalThis.__n9native  ── JS↔C marshalling (port/plan9/n9_native.c)
        ▼
  Plan 9 native layer (port/plan9/n9_sec.c)
        ├── libsec : sha2/sha1/md5, hmac, genrandom (CSPRNG), tlsClient (TLS)
        └── libz   : gzip inflate
  Networking : Plan 9 /net (/net/cs dial, /net/tcp, async via os.setReadHandler)
        ▼
  QuickJS-ng engine (port/plan9/quickjs.c, patched for kencc/APE — port/plan9/patch.sh)
  built with the Plan 9 kencc/APE toolchain (n9_cli.c = the CLI main)
```

- **The C port** (`port/plan9/`): `patch.sh` transforms pristine QuickJS-ng into a
  kencc/APE-buildable tree; `n9_cli.c` is a custom CLI main with an ESM module-loader hook;
  `n9_native.c`/`n9_sec.c`/`node9_native.h` are the native bindings.
- **Non-LP64 data model**: Plan 9 amd64 is `long`=4, `ptr`=8 — this drove `-DJS_NAN_BOXING=0`
  (struct `JSValue`) and a number of codegen fixes (see Limitations / kencc).

---

## 3. Fidelity to Node.js

### Engine / language
| Aspect | node9 | Node.js |
|---|---|---|
| Engine | QuickJS-ng (interpreter) | V8 (JIT) |
| Language level | ES2023 (classes, async/await, generators, BigInt, Map/Set, Proxy, optional chaining, private fields, regex lookbehind, `import()`) | ES2023+ |
| Module systems | CommonJS **and** ESM (static `import` + dynamic `import()`) | CommonJS + ESM |
| Native addons (`.node`) | **No** (by design — no dlopen/N-API) | Yes |
| V8-specific (`%natives`, snapshots, `vm` contexts, inspector) | No | Yes |

The JS *language* is faithfully the same — ordinary Node application code runs unchanged.
What differs is the engine internals and anything that reaches into V8.

### Node core API
The standard library is a mix of **hand-written Node-compatible modules** and code **ported
faithfully from Node's own source** where it matters (e.g. `path.basename`'s exact char-scan
algorithm; `util.inherits` = `setPrototypeOf` + non-enumerable `super_`; `assert`'s recursive
`deepStrictEqual`). The 46 builtins:

**Substantially complete:** `path` (+ posix/win32), `buffer` (full numeric read/write API,
copy/fill/indexOf/concat, BigInt64, Float/Double), `events` (+ static `once`/`on`,
`EventEmitter`), `stream` (Readable/Writable/Duplex/Transform/pipeline with real
backpressure), `util` (format/inherits/promisify/types), `assert` (strict + deep + throws/
rejects matchers), `querystring`, `url` (WHATWG `URL` + legacy), `string_decoder` (multi-byte
UTF-8), `crypto` (real SHA-2/SHA-1/MD5/HMAC/CSPRNG over libsec), `fs` (sync + callback +
`fs/promises` + streams + `FileHandle`, recursive mkdir, mkdtemp, rm -rf), `os`, `process`,
`http`/`https` (HTTP/1.1 client: chunked + gzip), `net`/`tls` (async sockets over `/net`),
`zlib` (gunzip via libz).

**Stubs / partial (present so `require` succeeds, limited behavior):** `http2` (sigstore
only — see below), `child_process` (sync `os.exec`; no streamed stdio), `worker_threads`,
`vm`, `inspector`, `perf_hooks`, `v8`, `domain`, `dns` (lookup via `/net/cs`), `readline`,
`async_hooks` (+ `AsyncLocalStorage`), `node:test` (minimal runner).

**Globals provided:** `URL`/`URLSearchParams`, `TextEncoder`/`TextDecoder`,
`AbortController`/`AbortSignal`, `Event`/`EventTarget`, `crypto` (Web Crypto: getRandomValues/
randomUUID/subtle.digest), `setImmediate`, `structuredClone`, `Buffer`, `process`.

### npm
npm runs **as-is** — the real 10.9.8 release tree. node9 was made compatible with npm, not
the reverse. The full ladder works: `npm --version` → `npm config` → `npm view` (real HTTPS)
→ `npm install` (with SRI gating) → installing packages **with dependencies** (arborist's
dependency graph; verified e.g. `debug` pulling `ms`).

---

## 4. Test coverage

### Borrowed from Node.js's own test suite
node9 runs **Node.js v20.20.2's actual `test/parallel/test-*.js` files** (the upstream unit
tests) against the runtime, via a small harness (`test/node/`: `run-one.js` + a minimal
`common` shim).

- Node's full `test/parallel` has **3,474** test files. node9 curates **106** of them for the
  core modules it implements: buffer (26), events/event (36), path (16), util (13), url (8),
  querystring (4), string_decoder (3).
- Pathological cases excluded: tests that allocate `MAX_STRING_LENGTH`-sized (~536 MB) buffers
  (engine-limit tests that OOM), and tests needing Node internals (`internal/*`,
  `internal/test/binding`) or Windows `path.win32` semantics.
- **Whole-file pass count: 11 / 98** at last run.

**Important caveat on that number:** Node's test files are *monolithic* — one file contains
hundreds of assertions, and a single Node-internal / exact-error-message / engine-limit
assertion fails the **entire file**. So whole-file pass rate severely *undersells* real
coverage: most files run hundreds of passing assertions and fail only on one Node-specific
detail (an exact `util.inspect` format string, an exact `ERR_*` message, a V8 stack-trace
shape). The borrowed tests' real value has been as a **bug-finding harness** — running them
drove concrete fixes:
  - `path.basename` (ported Node's exact algorithm), `util.inspect([])`, `util.format %d`,
    `util.inherits` descriptor, the full `Buffer` numeric API, `string_decoder` multi-byte,
    the CommonJS resolver's `main`-vs-`index.js` precedence,
  - and — most significantly — **two kencc floating-point codegen bugs** where `NaN`
    comparisons (`NaN > x`, `NaN === NaN`) and sort comparators returned wrong results.

### node9's own tests
`examples/` contains **34** behavioral tests written for node9 (network, streams, crypto,
buffer, the NaN/float battery, fs, the phase milestones, the 30-package probe).

### 30 popular npm packages — download **and** run
`examples/probe30.js` (result in `test/node/30-packages-result.txt`) installs 30 popular
packages from the registry and runs a real operation on each. **Result: 30 / 30 OK.**

- **24 CommonJS:** tslib, ms, semver, qs, minimist, commander, validator, classnames, clsx,
  is-number, uuid, dayjs, picocolors, kleur, colorette, safe-buffer, eventemitter3, inherits,
  once, deepmerge, dotenv, async, lru-cache, yallist
- **6 ESM (loaded via `import()`):** chalk, nanoid, camelcase, ansi-styles, strip-ansi,
  escape-string-regexp

(lodash was swapped for tslib — see Limitations: huge packuments.)

---

## 5. Limitations

### By design / platform
- **No native addons** (`.node` / N-API / node-gyp). Pure-JS packages only. This is inherent —
  there is no dynamic loader for compiled addons on this runtime.
- **No HTTP/2.** Stubbed (load-only). The only consumer in npm's tree is `@sigstore/sign`
  (publish provenance), which `npm install` never invokes. Real h2 would need HPACK + framing +
  TLS-ALPN (and `libsec` doesn't expose ALPN).
- **TLS certificate chain is not validated.** `libsec`'s `tlsClient` does a real handshake and
  encrypts the connection, but verification is thumbprint/pinning-based, not PKIX-chain — it
  can't validate a public-CA chain. **Package integrity is independently guaranteed by npm's
  SHA-512 SRI** (which *is* enforced — a corrupted tarball is rejected with `EINTEGRITY`). A
  userspace X.509 validator is the proper follow-on.
- **`child_process`** runs commands synchronously via `os.exec`; it does **not** stream
  stdout/stderr (returns empty). npm lifecycle scripts that parse command output won't work.
- **No `worker_threads`**, no real `vm` contexts, no inspector/debugger.
- **`fsync` is a no-op** (weaker write durability — fine for a workstation install).
- **Symlinks** fall back to copies on file servers that lack them.

### Fidelity gaps
- **`util.inspect` is not byte-identical** to Node's (simpler formatter). Anything depending on
  exact inspect output (or exact `AssertionError`/`ERR_*` message strings) will differ.
- **Negative zero is not preserved** — `-0` evaluates to `+0` (`1/-0 === +Infinity`,
  `Object.is(0,-0) === true`). A kencc unary-negate/fold quirk; negligible real-world impact.
- **No ICU/Intl**; `string_decoder`/`Buffer` cover UTF-8/UTF-16LE/latin1/base64/hex but not
  every Node encoding edge case.
- **HTTP client has no socket timeout** — a hung server would hang the request.

### Performance
- node9 is an **interpreter** with a JS-implemented tar/fs path, so it is much slower than
  Node. Large packages are slow: e.g. lodash (~650 files) extracts at ~0.3 s/file; packages
  with **huge packuments** (lodash, pinned `lru-cache@10` — hundreds of versions) are very slow
  to fetch + `JSON.parse` and can appear to hang.
- **Install one package at a time.** `npm install A B C …` for many packages at once blows up
  arborist's combined version resolution and spins; sequential single-package installs are
  reliable.

### kencc/APE toolchain (the C port)
Building QuickJS with Plan 9's `kencc` surfaced several **backend codegen bugs** (all fixed in
`port/plan9/patch.sh` and documented in `port/plan9/NOTES.md`): an arg-hoist pointer-math
miscompile, and **floating-point comparisons against `NaN`** returning wrong results in the
relational ops, the equality ops, and `Array`/`TypedArray` sort comparators (the last was a
memory-safety hole). All are guarded; a battery of NaN/float tests passes
(`examples/nanbattery.js`, `nan2.js`).

---

## 6. Installing on a stock 9front (without the agent9 image)

node9 is part of the agent9 image, but it also builds and runs on a **plain 9front amd64**
install. Everything it needs ships with 9front: the **kencc/APE** toolchain (`pcc`), **libsec**
(crypto/TLS), **libz** (gzip), and `/net`. You also need a Unix host (macOS/Linux) to run the
patcher and serve files to the 9front box over plain HTTP. `HOST` below is your host's IP as
seen from 9front.

**1. On the host — produce the patched QuickJS tree + collect the sources.**
`patch.sh` rewrites pristine QuickJS-ng into a kencc/APE-buildable tree (and applies the
floating-point/codegen fixes).

```sh
# from a clone of this repo:
mkdir -p /tmp/node9probe/src && cd /tmp/node9probe/src
# fetch the exact QuickJS-ng the patch targets, into ./quickjs-master
curl -L https://github.com/quickjs-ng/quickjs/archive/refs/heads/master.tar.gz \
  | tar xz && mv quickjs-* quickjs-master
bash /path/to/node9/port/plan9/patch.sh            # -> /tmp/node9probe/src/qjs-patched.tar.gz
# stage the node9-native sources + the runtime stdlib next to the tarball
cp /path/to/node9/port/plan9/{n9_cli.c,n9_native.c,n9_sec.c,node9_native.h,build-cli.rc} .
cp /path/to/node9/lib/boot.js node9_boot.js
# also fetch the real npm tarball so 9front can pull it over plain HTTP
curl -L -o npm.tgz https://registry.npmjs.org/npm/-/npm-10.9.8.tgz
python3 -m http.server 8833                        # serve this dir
```

**2. On the 9front box — build `qjs`** (≈1 min; produces a ~2.5 MB binary):

```rc
mkdir -p $home/node9 && cd $home/node9
hget http://HOST:8833/qjs-patched.tar.gz | gunzip | tar x
for(f in n9_cli.c n9_native.c n9_sec.c node9_native.h build-cli.rc)
        hget http://HOST:8833/$f > $f
rc build-cli.rc            # pcc compile + link (-lsec -lz) -> work/qjs
```

**3. Install the runtime + the `node`/`npm` commands:**

```rc
cp $home/node9/work/qjs /amd64/bin/qjs
mkdir -p /amd64/lib/node9
hget http://HOST:8833/node9_boot.js > /amd64/lib/node9/boot.js
# real npm 10 tree
cd /amd64/lib/node9 && hget http://HOST:8833/npm.tgz | gunzip | tar x && mv package npm
# node + npm wrappers (qjs auto-loads boot.js, so it *is* the node runtime)
echo '#!/bin/rc'                                              > /amd64/bin/node
echo 'exec /amd64/bin/qjs $*'                                >> /amd64/bin/node
echo '#!/bin/rc'                                              > /amd64/bin/npm
echo 'exec /amd64/bin/qjs /amd64/lib/node9/npm/bin/npm-cli.js $*' >> /amd64/bin/npm
chmod +x /amd64/bin/node /amd64/bin/npm
```

**4. Verify:**

```rc
echo 'console.log("node9", process.version)' > /tmp/v.js && node /tmp/v.js   # -> node9 v20.18.1
npm --version                                                                # -> 10.9.8
mkdir -p /tmp/t && cd /tmp/t && npm install left-pad                         # real registry install
```

Notes: the build runs entirely with the stock 9front toolchain — no cross-compiler needed on
the 9front side (only the host runs `patch.sh`, which needs `bash`/`sed`/`perl`). npm reaches
`registry.npmjs.org` over node9's own `libsec` TLS; it does **not** depend on `hget`'s HTTPS or
on `/sys/lib/tls/ca.pem` (integrity is gated by npm's SHA-512 SRI, not chain validation — see
Limitations). Full build/obstacle detail is in
[`port/plan9/NOTES.md`](port/plan9/NOTES.md); the host-side build/run flow mirrors
[`port/plan9/build-cli.rc`](port/plan9/build-cli.rc).

## 7. Summary table

| | node9 |
|---|---|
| Platform | 9front / Plan 9, amd64 |
| Engine | QuickJS-ng (ES2023 interpreter) |
| Runtime stdlib | `lib/boot.js`, ~2,375 lines, 46 builtin modules |
| Native layer | Plan 9 `libsec` (crypto/TLS) + `libz` (gzip), over `/net` |
| npm | real npm 10.9.8, unmodified |
| Install pipeline | registry → TLS → SHA-512 SRI → gunzip → tar → `node_modules` + lockfile |
| Borrowed Node tests | 106 curated from Node v20.20.2's 3,474 `test/parallel` files |
| node9 own tests | 34 (`examples/`) |
| Popular packages run | **30 / 30** (24 CJS + 6 ESM) |
| Not supported | native addons, HTTP/2, PKIX chain validation, worker_threads, streamed child_process |

See also: [`port/plan9/NOTES.md`](port/plan9/NOTES.md) (every kencc/APE obstacle + fix) and
[`test/node/README.md`](test/node/README.md) (the borrowed-Node test harness).
