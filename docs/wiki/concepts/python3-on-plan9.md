---
title: Python 3 on Plan 9 — port strategy + parity harness
created: 2026-06-18
updated: 2026-06-18
type: concept
tags: [toolchain, cross-compile, status-wip, fs]
---

# Python 3 on Plan 9

Effort to bring **CPython 3.11** to 9front/amd64, validated against CPython's
own regression suite rather than by inspection. Lives under `python9/` in the
repo. Complements [[build-toolchain]] and [[llm-porting-workflow]].

> **Why this exists.** The north-star is running the Nous `hermes-agent` on
> 9front. See the honest blocker analysis below — a working interpreter is a
> *prerequisite, not the finish line*. Contrast with [[pi9-architecture]],
> which deliberately went the other way (Go-native agent, "not Hermes-on-plan9").

## State of the art (what already exists)

- **Python 2.5.1** historically shipped with 9front (only because Mercurial
  needed it). Archived in 9front's `pyhg` repo.
- **CPython 2.7.5** — real port by Jeff Sickel ("jas", github.com/vat9),
  sponsored by Coraid, ~2013. `plan9` branch off 2.7; added a native `_p9ssl`
  on libsec/`ssl(3)`/`tls(3)`. 386/amd64. **No longer maintained.**
- **Python 3** — never finished. The 2013 9fans "Python3 for Plan9" thread is
  the high-water mark (3.3.2 cross-compiled for ARM, never tested). **No
  maintained CPython 3 port for 9front exists today.**
- **Cosmopolitan "APE-python"** (jart) builds 2.7.18/3.6.14 fat binaries — but
  targets Linux/Mac/Windows/BSD, **not Plan 9**. Name collision: Cosmopolitan
  "APE" (Actually Portable Executable) ≠ Plan 9 "APE" (ANSI/POSIX Environment).

## Why 3.11

Lowest version `hermes-agent` allows (`requires-python >=3.11,<3.14`), so it
minimizes the interpreter delta while staying in range. Host reference pinned to
**3.11.14** so host and port run byte-identical `Lib/test`.

## Parity is the contract

The bar is **not** "100% of the suite" — no CPython port reaches that on any
platform (CPython has tiered platform support). Measurable bar:

> Of every testcase that passes on the reference 3.11.14 build and is not on the
> justified skip-list, what fraction passes on the 9front port?
> `parity = port_pass / applicable_ref_pass`

Harness (`python9/parity/`): `run_suite.py` (portable, stdlib-only, 3.11 syntax —
runs identically on host and in the VM) emits a normalized JSON manifest from
`python -m test --junit-xml`; `score.py` diffs port vs reference and reports the
parity score + regression work-queue; `skiplist.txt` is curated and justified.

**Reference baseline captured:** 38,259 passing testcases on 3.11.14
(darwin/arm64), 2m08s. That is the denominator. `test_distutils` fails on the
host and self-excludes (parity only counts ref-passing tests).

## APE build environment (recon, 9front GEFS SP1, amd64)

Verified live in the dev VM (`sysname cirno`):

- **Compilers:** `pcc` (APE C driver → `6c`/`6l`), native `6c`/`6l`.
- **APE utilities — only 12:** `basename cc dirname expr kill make psh sed sh
  stty tar uname`. **No `ls`/`pwd`/`grep`/`tr`/`cat`/`mkdir`/`rm`.**
- **Headers:** full `/sys/include/ape/*` (stdio, unistd, bio, draw, …).
- **SSL:** `/amd64/lib/ape/libsec.a` + `libsec.h` + `mp.h` present → native
  `_ssl`/`_p9ssl` path viable (reuse jas's approach).
- **Missing — the two hard constraints:**
  - **No `dlfcn.h`** → no dynamic loading. All C extensions must be **static**,
    listed in `Modules/Setup` and linked into the interpreter. `importlib`
    dynamic `.so` loading is out.
  - **No `pthread.h`** → CPython 3.7+ makes threads mandatory. Need a **Plan 9
    thread backend** (`Python/thread_plan9.h`: `rfork(RFMEM)`/`proccreate` +
    locks). This is the single biggest interpreter-side task and what jas had
    to solve for 2.7.

## Decision: bypass `configure`

CPython's autoconf `configure` **does not run on this APE**. Confirmed: it
shells out to `ls -di .`/`pwd`/`grep`/`tr`/etc., APE ships none of those, the
PATH falls back to native Plan 9 tools whose flags differ, and configure dies at
`working directory cannot be determined` (native `ls` rejecting autoconf flags).
Shimming a full POSIX coreutils is a rabbit hole.

**Strategy = hand-author `pyconfig.h` + a custom `mkfile`** (6c/6l or pcc
directly) with a static `Modules/Setup`. This is what jas's 2.7 port and the
historical 9front python did. Do **not** fight autoconf.

## Hermes reality check (the real blocker)

Porting the interpreter does **not** make `hermes-agent` run. Its 219-package
tree includes **Rust-backed** extensions — `pydantic-core`, `jiter`,
`cryptography`, `rpds-py`, `tokenizers`, `watchfiles` — which **cannot compile
on Plan 9 (no rustc target)**, plus OS-locked C extensions (`uvloop` = libuv,
`psutil`, `httptools`). Running Hermes would require a de-Rusted fork
(pydantic v1 pure-Python, stdlib http instead of aiohttp/uvloop). The Go-native
route ([[pi9-architecture]]) remains the cheaper path for the agent itself.

## VM workflow gotchas (this port specifically)

See also [[testing-harness]] for listen1 pitfalls. Hard-won here:

- **Single-line, absolute-path commands only** over listen1. Multi-line, `&&`,
  and `;`-chains silently return empty on the slow TCG VM.
- **`hget` has no `-o`** on this build — use shell redirect:
  `hget URL > /abs/path`.
- 25 MB source transfer: host `python3 -m http.server`, VM
  `hget http://10.0.2.2:PORT/... > /usr/glenda/py.tgz`, then
  `gunzip < py.tgz | tar x`.

## Status / next

- [x] Parity harness + reference baseline (committed, branch
  `python9-parity-harness`).
- [x] APE env recon; source staged at `/usr/glenda/Python-3.11.14` in dev VM.
- [x] Confirmed `configure` is a dead end → hand-authored build.
- [ ] Author `pyconfig.h` + mkfile; get a stripped `python` to build with 6c/6l.
- [ ] Plan 9 thread backend (`thread_plan9.h`).
- [ ] Boot REPL in VM → run `run_suite.py` → first parity number.
