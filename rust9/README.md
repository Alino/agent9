# rust9 — Rust on 9front

Rust running on a **stock 9front amd64 kernel** — both ways: cross-compiled from
the host (same ethos as [`cc9`](../cc9) and [`zig9`](../zig9)), and **the real
`rustc` running on 9front itself**, compiling, linking, and running programs
natively with no other machine involved.

## Install on the box (pac9)

```
pac9 install rust9        # ~80 MB download, ~210 MB installed
```

Then, on 9front:

```
echo 'fn main() { println!("hi from rustc on plan9") }' > hi.rs
rustc hi.rs -o hi
./hi
```

The package is **self-contained**: the `rustc` a.out (1.98-dev, cranelift
backend), the plan9 std sysroot, the [`n9link`](../cc9/host/n9link.c) ELF→a.out
linker, the `cargo9` mini-cargo (build/run/clean for local-path workspaces), and
the cc9 runtime substrate it links against — all under `/usr/glenda/rust`, with
`rustc`/`cargo9` wrappers in `/rc/bin`. It depends on **no other pac9 package**
(you do not need `cc9`). `pac9 uninstall rust9` removes every installed file.

rc gotcha: `=` is rc syntax, so quote flags that contain it —
`rustc '--emit=metadata' x.rs` or use the space form `--emit metadata`.

On-box limits (see `RUSTC-PORT.md`): no crates.io/proc-macros (single files and
local-path deps via `cargo9`), one cranelift opt tier regardless of `-O`, no
`asm!`, empty backtraces. For anything heavier, cross-compile from the host
(below) — it's the same std and linker.

## How it works

rustc is LLVM-based, and its stock **`x86_64-unknown-none`** target emits exactly
the freestanding SysV objects the cc9 pipeline already consumes. So rust9 adds no
runtime of its own — it reuses cc9 wholesale:

```
rustc --target x86_64-unknown-none --crate-type staticlib   # -> libfoo.a (core [+alloc])
  |
ld.lld  + cc9/test/plan9.ld  + cc9/lib/libcc9cxx.a (crt0/_start, syscalls, malloc, libc)
  |                            + cc9/lib/libcc9m.a (openlibm)
cc9/host/elf2aout.py   # ELF -> Plan 9 a.out
  |
cc9/host/deliver.py    # ship over listen1, run on the VM
```

cc9's `crt0` owns `_start` (FP-exception masking, argv, note handler, `.init_array`),
then calls `int main(int, char**)` — which the Rust staticlib provides via
`#[no_mangle] extern "C" fn main`.

## Status

- **`core` ✓** — `test/hi.rs` prints on 9front (stock `x86_64-unknown-none`, no nightly).
- **`alloc` ✓** — `test/alloc.rs`: `Vec` + iterators + `format!`/`String`, heap wired
  to cc9's thread-safe `posix_memalign`/`free`. Prints `sum of squares 1..8 = 204`.
- **`std` ✓** — a real `std::sys::pal::plan9` runs on stock 9front, needing the custom
  **`x86_64-unknown-plan9`** target + nightly `-Zbuild-std`. Working: `println!`/`File`/
  `read_dir`/`env`/`args`/`Instant`, **real threads + `Mutex`/`Condvar`/`RwLock`/channels**
  (cc9 pthreads), **`panic = unwind`** (`catch_unwind` on cc9's DWARF unwinder), and
  **`std::process`** — spawn/wait + inherit/null/pipe stdio + `Command::output()` (rfork+exec).
  The std patch is a version-pinned overlay in `std/` (`std/apply.sh` / `snapshot.sh`).
- **test suite ✓** — Rust's own upstream `coretests` (2771 tests) built via `-Zbuild-std`
  and run on the box: **~2309 pass, 0 Rust-side failures** (the 1 failure is a cc9 openlibm
  `tgamma` edge case, not the port). Run in module chunks (`host/run-suite.py`) — cc9 has a
  per-process thread-churn limit (~1024) since libtest spawns a thread per test.
- **flagship ✓** — `rgrep/` (recursive grep on the real crates.io `regex`), and
  **`minirustc/` — a Rust compiler that runs on 9front** (compiles a Rust subset to a native
  a.out via the on-box `6c`/`6l`, driven through `std::process`).
- **the real `rustc` on 9front ✓** — the full upstream compiler (`1.98.0-dev`) cross-built
  as a static Plan 9 a.out with the cranelift backend linked in (no dlopen on Plan 9), plus
  `n9link`, a from-scratch static ELF→a.out linker that runs on the box. `rustc hi.rs`
  self-hosted on bare metal. Packaged: `pac9 install rust9` (see above);
  build recipe in `release/make-tarball.sh`, port log in `RUSTC-PORT.md`.

`net` is real (TCP client+server and UDP over `/net`, plus TTL); `process` has
`try_wait`, piped `output()` with a concurrent drain, and per-thread errno underneath;
fs has chmod/mtime/canonicalize (gate tests: `test/gap_*.rs`). Still stubbed honestly:
socket timeouts / `set_nonblocking`, `peek`, multicast, `File::lock`, `fsync`,
`env_clear`, `backtrace` (empty). The full `rustc` port lives in `RUSTC-PORT.md`.

## Use (host cross-compile)

**Single-file, no nightly** (core/alloc only, via stock `x86_64-unknown-none`):
```
rustup target add x86_64-unknown-none
host/rust9 run test/alloc.rs                 # build + run on the 9front VM
```

**Full std, via cargo + build-std** (nightly):
```
rustup toolchain install nightly --component rust-src
std/apply.sh                                 # install the plan9 std backends into rust-src
cd stdhello && cargo +nightly build --release       # build-std of std for x86_64-unknown-plan9
python3 ../host/run9.py target/x86_64-unknown-plan9/release/stdhello   # deliver + run (with args)
```

Gotcha: `-Zbuild-std` does NOT detect edits to sysroot rust-src — `cargo clean` after
any `std/` change. Config via env: `RUST9_TARGET`, `CC9`, `CC9_LLD`, `CC9_DEV`
(`"host port"`, default `127.0.0.1 1717`).

Prereqs: `cc9/lib/libcc9cxx.a` (built by `cc9/host/build-runtime.sh`) and either
`rustup target add x86_64-unknown-none` (single-file) or nightly+rust-src (std).
