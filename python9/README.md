# python9 — CPython 3.11 for 9front

CPython 3.11.14, built for 9front/amd64 with the native toolchain: kencc (6c/6l) driven by APE's pcc, no autoconf. It boots, runs the REPL and the standard library, and passes CPython's own regression suite at 100% parity on the core batch (6120/6120, 0 regressions against the host 3.11.14 reference). Build notes and the bug-class write-up live in [`port/plan9/README.md`](port/plan9/README.md).

## How it was built (disclosure)

This port was done with heavy AI assistance: I used Claude (and Hermes) to drive the patch / compile / test loop and to track down the kencc and APE bugs. The engineering is real and it's documented, but the commit history reflects that workflow, and I'd rather say so up front than have you find it in the log.

## What works, and what doesn't

Works: the interpreter, the pure-Python standard library, and the modules covered by the core/numeric parity batch.

Not yet: it's single-threaded. CPython's pthread stub, not a native `threads(2)` backend, which is the next piece of work. There's no `ssl` module yet. `_socket` compiles via APE but networking isn't part of the parity claim. Anything that needs a C or Rust toolchain, or OS primitives Plan 9 doesn't have, won't build: numpy, cryptography, pydantic-core and friends are out.

Why 3.11: it's the lowest version the original target workload (hermes-agent) allows (`requires-python >=3.11,<3.14`), which keeps the interpreter delta small. The host reference is pinned to 3.11.14 to match exactly.

> Reality check: a working interpreter does not make hermes-agent run. Its dependency tree pulls in Rust-backed extensions (`pydantic-core`, `jiter`, `cryptography`, `rpds-py`, `tokenizers`, `watchfiles`) with no rustc target on Plan 9, plus OS-locked C extensions (`uvloop`, `psutil`, `httptools`). The interpreter is a prerequisite, not the finish line.

## Parity is the contract

"100% of the suite" is the wrong bar. No CPython port hits that on any platform. Here's the bar that actually means something:

> Of every testcase that passes on the reference 3.11.14 build and isn't on the justified skip-list, what fraction passes on the 9front port?

The oracle is CPython's own regression suite (`Lib/test`, `python -m test`), the same suite the core team gates releases on. The host reference and the 9front port run the same vendored `Lib/test` tree through the same `run_suite.py`, so any difference is the platform, not version skew.

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
