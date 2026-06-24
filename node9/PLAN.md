# node9 — a Node-compatible JavaScript runtime for 9front

`node9` brings a Node.js-flavored runtime to 9front/amd64, sitting alongside
[`python9`](../python9) and [`pi9`](../src/pi9). It is **not** a port of Node.js
itself — see the Phase 0 findings for why that's impractical — but a
Node-compatible runtime built on the **QuickJS** engine with a Plan 9-native
event loop and a subset of Node's standard library.

---

## Phase 0 — feasibility (DONE, 2026-06-24)

Probed empirically on bare-metal amd64 9front (`cirno`). Full toolchain facts in
the `plan9-c-toolchain-capabilities` memory.

| Killer | Question | Verdict |
|---|---|---|
| #1 Toolchain | C++17 → working 9front amd64 binary? | 🔴 V8 — no C++ compiler exists at all (`6c` is C-only; no g++/gcc/clang). 🟢 QuickJS — `pcc`/kencc compiles, links, and runs amd64 binaries; full APE installed. |
| #2 QuickJS | Builds under APE? | 🟢 GREEN with bounded patches. kencc supports more C99 than expected (designated init, compound literals, anon unions, for-decls, stdbool). Only real work: replace `alloca`, rewrite a few anon-union designated-inits. |
| #3 V8 substrate | mmap / pthreads / W^X? | 🔴 No `mmap`, no `pthreads` in APE. V8's `platform-posix.cc` is unusable. Native `segattach` + `rfork(RFMEM)` exist but require a from-scratch `platform-plan9.cc`. **Moot for QuickJS.** |

**Decisive insight:** QuickJS is a *bytecode interpreter* (no JIT), so it needs
none of the executable-memory / W^X / `mmap` machinery that makes V8 a substrate
nightmare on Plan 9. The thing that kills V8 is the thing QuickJS doesn't use.

**Data-model hazard (confirmed live):** `long=4, ptr=8, int=4` — not LP64. See the
`python9-plan9-datamodel` memory. QuickJS NaN-boxing / pointer-tagging will hit this.

**Decision:** Build node9 on QuickJS (Phases 1–5). Real-V8 track shelved —
blocked twice (no C++ toolchain anywhere in the Plan 9 world; no mmap/pthread
substrate). Both are prerequisites that cannot be parallelized away.

---

## Parity methodology (adopted after Phase 3 v1)
Reference = **Node.js v24.18.0 (LTS)**, used for genuine 1:1 API/behavior parity — *not*
reimplemented from memory. Node core is mostly JS (`lib/*.js`) over a thin native
`internalBinding` seam. So:
- **Adapt Node's real `lib/*.js` verbatim** (MIT, license preserved in `nodelib/NODE_LICENSE.txt`)
  → near-perfect fidelity for the JS parts.
- **Reimplement only the binding seam Plan 9-natively** — this is where the Plan 9 mindset
  lives: `fs`/`os` → quickjs-libc os; `net`/`dns` → `/net`,`/net/cs`; `child_process` → rfork.
- **Bootstrap** (node9-original, not from Node): `nodelib/primordials.js` (generic intrinsic
  shim, serves every module) + small `internal/{constants,errors,validators,util}.js`.
- **Verify against Node's own behavior/tests.**

Proven end-to-end (real Node lib running **unmodified** on node9, 1:1): `path` 24/24,
`events` 16/16, `os` 17/17 (over a Plan 9-native `internalBinding`). Bootstrap + adapted
modules in `nodelib/`. Full node9 suite **9/9 green** (`port/plan9/run-all-tests.rc`).
The hand-written `lib/boot.js` modules (Phase 3 v1) are being superseded by these.

Cracking `os` required fixing a **kencc backend codegen bug** (non-LP64): it distributes a
`*sizeof` multiply such that `var_idx*24 == 0x300000000`, corrupting an arg-hoist store
address. Fixed with a bitmask barrier on the index (see `port/plan9/NOTES.md`; repro at
`examples/repro-arghoist-crash.js`). This is a compiler-level fix that benefits *all* code.

## Engine baseline
**quickjs-ng** (the maintained fork) over Bellard's original — better ES2023+
coverage and active fixes. Reference for the overall architecture (QuickJS +
event loop + Node-ish API): **txiki.js**, but with the libuv layer *replaced* by a
Plan 9-native loop.

---

## Phase 1 — engine bring-up  *(milestone reached: qjs runs JS)*
Real JavaScript executing on 9front; no Node API yet.
- [x] Fetch quickjs-ng v0.15.1, build the full engine (quickjs.c 63.5k lines, libregexp,
  libunicode, dtoa) clean to `.6` objects on kencc/APE.
- [x] **Link a minimal `n9_main.c` + engine → `qjs`, which runs modern JS.** Verified
  closures, recursion, regex (capture groups + global replace), Unicode case folding,
  try/catch, Map/Set/spread, JSON, **BigInt**, template literals (`test.js`).
- [x] Found & fixed the data-model bug: APE's 32-bit `intptr_t` mis-triggered NaN-boxing
  → pointer truncation. Fix: `-DJS_NAN_BOXING=0` (struct JSValue). See `port/plan9/NOTES.md`.
- [x] **Compile `quickjs-libc.c` (POSIX `std`/`os` modules) + build a real CLI (`n9_cli.c`).**
  `qjs` (1.85 MB, installed at `/amd64/bin/qjs`) runs scripts/modules + a REPL. Verified
  `console.log`, `std`/`os`, env, real file I/O, `os.getcwd()`. POSIX gaps (`poll`/`dlopen`/
  tty/`setenv`/…) stubbed to degrade gracefully. See `port/plan9/NOTES.md`.
- [ ] Replace the SMOKE `alloca`→malloc shim (leaks) with proper heap+free; restore the
  stack-overflow guard; tighten the approximate C99 math shims; un-stub the degraded os.*.
- [ ] Pass a QuickJS test262 subset.
- **Exit:** `qjs script.js` runs arbitrary modern JS on 9front. ✅ **Reached — with a CLI.**

## Phase 2 — native event loop  *(DONE)*
The async substrate, Plan 9-shaped (not a libuv port).
- [x] Event loop = quickjs-libc `os` module + `js_std_loop`. Timers run via `nanosleep`
  (no `poll` needed). Async fd I/O via a real `poll()` implemented over **APE `select()`**
  — which multiplexes with Plan 9 helper procs internally (the rfork-based native loop,
  via a tested path). `port/plan9/poll.h` is now a ~45-line poll↔select translation.
- [x] Verified: `setTimeout`/`setInterval` ordering; `os.pipe`+`setReadHandler` async read
  fires on the loop; Promises/`async`-`await`/`Promise.all`; microtask-before-timer order.
- **Exit:** a timer fires and an async read's callback runs on the loop. ✅ (`async-test.js`)

## Phase 3 — Node API surface  *(Tier 1 DONE)*
- [x] Tier 1: `fs` (sync+async+promises), `process`, `path`, `events`, `buffer`,
  `stream` (minimal), `util`, `os`, plus `assert`. All Node-doc examples pass.
- [x] CommonJS `require`: builtins + relative file modules + JSON + `node:` scheme +
  `__dirname`/`__filename` + `require.resolve`/`cache`. Implemented in JS as
  [`lib/boot.js`](lib/boot.js) (489 lines), loaded at qjs startup from
  `/amd64/lib/node9/boot.js` (override via `NODE9_BOOT`). Globals: `require`, `process`,
  `Buffer`, `global`, `setTimeout`/`setInterval`. Examples in [`examples/`](examples/).
- [ ] Tier 2: `net`/`dns` (via `/net/cs`+`/net/dns`), `child_process` (rfork/exec),
  `crypto` (bind APE `libsec`). (`net` lands with Phase 4 http.)
- [ ] ESM loader for bare builtin specifiers (`import` of builtins; file ESM already works).
- **Exit:** Node-doc examples for Tier 1 pass. ✅ (`examples/phase3-test.js`)

## Phase 4 — HTTP & the "it's actually Node" milestone
- `http`/`https` server + client on the Phase 2 net layer.
- Flat `node_modules` resolution (no native addons).
- **Exit:** `curl` from another host hits a node9 HTTP server and gets a response.

## Phase 5 — compatibility, hardening, real apps
- Run a slice of the Node test suite into a tracked pass/fail matrix.
- Make 3 real npm packages (no native deps) run.
- Perf + leak pass; release artifact like the agent9 image.
- **Exit:** documented compatibility matrix + 3 real packages running.

---

## Build/test workflow
Same pattern as python9 ([`python9-port-workflow`] memory): live 9front target,
`listen1` command channel, `hget` file transfer from a host `http.server`, `acid`
for fault backtraces. Iterative builds run against the **QMP-reboot-able dev VM**
(not bare-metal `cirno`, which wedges on a faulting child → physical power-cycle).

## Critical path
`0 → 1 → 2 → 3 → 4`. Phase 3 modules parallelize heavily. Phases 1–2 are the
serial spine.
