# node9

A **Node.js-compatible JavaScript runtime for 9front/Plan 9 (amd64)** — and the
**real, unmodified npm** running on top of it. `npm install` packages from the public
registry and `require()`/`import()` them on Plan 9, a platform Node has never supported.

Not a port of Node.js itself: Node = V8 + libuv, and V8 is impractical on Plan 9 (no
C++ toolchain, no `mmap`/`pthread`/JIT substrate). node9 is built on
[**QuickJS-ng**](https://github.com/quickjs-ng/quickjs) — a small ES2023 bytecode
interpreter that sidesteps every one of those blockers — plus a Node-compatible
standard library (`lib/boot.js`, 46 builtin modules), native bindings over Plan 9
`libsec` (crypto/TLS) and `libz` (gzip), and async networking over `/net`.

## Status: npm works

The actual **npm 10.9.8** runs on node9. Verified end to end:

```
npm install left-pad   →  fetch over real TLS → SHA-512 SRI-verify → gunzip → tar → node_modules + lockfile
npm install debug      →  pulls its `ms` dependency (arborist dependency graph)
```

- **30 / 30** popular packages download **and** run (24 CommonJS + 6 ESM): lodash-class
  utilities, chalk, uuid, semver, commander, dayjs, … (see
  [`test/node/30-packages-result.txt`](test/node/30-packages-result.txt)).
- Runs Node.js's own `test/parallel` unit tests as a compatibility + bug-finding
  harness ([`test/node/`](test/node/)).

## Docs

- [**DOCUMENTATION.md**](DOCUMENTATION.md) — what node9 + npm are, fidelity to the
  originals, **limitations**, how to install on a stock 9front, and test coverage.
  **Start here.**
- [`port/plan9/NOTES.md`](port/plan9/NOTES.md) — the kencc/APE port: every toolchain
  obstacle and fix, including the floating-point codegen bugs found and fixed.
- [`test/node/README.md`](test/node/README.md) — the borrowed-Node test harness.

## Limitations (short version)

No native addons (`.node`), no HTTP/2, no PKIX cert-chain validation (npm's SHA-512
SRI gates package integrity instead), limited `child_process`/`worker_threads`, and
interpreter-speed performance. Full list in [DOCUMENTATION.md](DOCUMENTATION.md).

Sibling projects: [`python9`](../python9) (CPython 3.11), [`pi9`](../src/pi9) (Go agent).
