# python9 — CPython 3.11 for 9front

A **test-suite-validated** CPython 3.11.14 port for 9front/amd64, the Plan 9 way
(libsec/`tls(3)` for SSL, `dial(2)` for sockets, `threads(2)`/`alt` where a POSIX
port would reach for pthreads/`select`).

**Status: the interpreter builds, boots, and scores 100.00% parity (6120/6120,
0 regressions) against the host 3.11.14 reference on the 39-module core batch.**
Build details and the bug-class archaeology are in
[`port/plan9/README.md`](port/plan9/README.md).

Why 3.11: it is the lowest version the target workload (hermes-agent) allows
(`requires-python >=3.11,<3.14`), so it minimizes the interpreter delta while
staying in range. The host reference is pinned to **3.11.14** to match exactly.

> Reality check: porting the interpreter does **not** make hermes-agent run.
> Its dependency tree includes Rust-backed extensions (`pydantic-core`, `jiter`,
> `cryptography`, `rpds-py`, `tokenizers`, `watchfiles`) that cannot be compiled
> on Plan 9 (no rustc target), plus OS-locked C extensions (`uvloop`, `psutil`,
> `httptools`). A working interpreter is a prerequisite, not the finish line.
> See the wiki concept page for the full analysis.

## Parity is the contract

The bar is **not** "100% of the suite" — no CPython port reaches that on any
platform. The measurable bar is:

> Of every testcase that passes on the reference 3.11.14 build and is not on the
> justified skip-list, what fraction passes on the 9front port?

The oracle is CPython's own regression suite (`Lib/test`, `python -m test`) — the
same suite core devs gate releases on. Both the host reference and the VM port
run **the same vendored `Lib/test` tree** via the same `run_suite.py`, so any
difference is the platform, not version skew.

## Layout

```
python9/
  cpython/src/         vendored CPython 3.11.14 source (gitignored; fetch script)
  cpython/host-build/  reference build output (gitignored)
  parity/
    fetch_cpython.sh        fetch the pinned source tarball
    build_host_reference.sh build the reference interpreter on the host
    run_suite.py            portable runner -> normalized JSON manifest
    score.py                diff port manifest vs reference -> parity score
    run_in_vm.sh            VM-side runner (scaffold until a port binary exists)
    skiplist.txt            curated, justified platform-impossible tests
    manifests/              committed JSON manifests (the scoreboard, over time)
```

## Workflow

```sh
# 0. one-time: fetch + build the reference oracle on the host
parity/fetch_cpython.sh
parity/build_host_reference.sh

# 1. capture the reference manifest (host)
parity/run_suite.py --python cpython/src/python.exe \
    --out parity/manifests/host-reference.json

# 2. (later) capture the port manifest inside the 9front VM
parity/run_in_vm.sh

# 3. score the port against the reference
parity/score.py --reference parity/manifests/host-reference.json \
    --port parity/manifests/9front-port.json \
    --skiplist parity/skiplist.txt
```
