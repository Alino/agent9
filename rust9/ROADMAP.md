# rust9 Roadmap — Rust on stock 9front

Rust running on an **unmodified 9front amd64 kernel**, cross-compiled on the host
and delivered over `listen1`, reusing the [`cc9`](../cc9) substrate (LLVM → Plan 9
a.out) wholesale. This file is the staged plan from "`core`+`alloc` run today" to
"a real Rust CLI ships on 9front."

---

## 1. Status

> **UPDATE — M1, M2, M3, M5 all DONE and proven on the 9front VM.**
> - **M2 ✓** custom `x86_64-unknown-plan9.json` + `rust9-ld` linker wrapper +
>   `cargo -Zbuild-std=core,alloc` (SSE on, hard-float ABI). `rust9/hello/`.
> - **M3 ✓** minimum-viable `std` = a real `std::sys::pal::plan9` over the cc9 runtime:
>   `println!`/`File`/`read_dir`/`env`(/env)/`args`/`thread`(cc9 pthreads)/`Instant`(/dev/bintime).
>   Patch is a version-pinned overlay in `rust9/std/` (`apply.sh`). `net`/`process`/real
>   `Mutex`/`Condvar`/`Parker` still stubbed; `panic = abort`.
> - **M5 ✓** flagship `rust9/rgrep/` — recursive regex grep on the real crates.io `regex`
>   crate, cross-compiled + run on 9front. (M4 cargo dev-loop is de-facto done: cargo +
>   `run9.py`.)
> The prose below is the original plan; the milestone bodies remain accurate references.

**M1 — `core` + `alloc` on stock 9front: DONE (this session).**

The one-line how: rustc's stock **`x86_64-unknown-none`** target emits freestanding
SysV ELF objects that the cc9 pipeline already consumes, so a Rust `staticlib`
exposing `extern "C" fn main` links straight against cc9's crt0/`_start`, syscall
thunks, `malloc`, and openlibm — **no rust9 runtime of its own**.

```
rustc --target x86_64-unknown-none --crate-type staticlib -C panic=abort
  └─ ld.lld + cc9/test/plan9.ld + cc9/lib/{libcc9cxx.a,libcc9m.a}
       └─ cc9/host/elf2aout.py  →  Plan 9 a.out  →  deliver.py → run on the box
```

Proven: `test/hi.rs` prints (`core`); `test/alloc.rs` runs `Vec` + iterators +
`format!`/`String` with the heap wired to cc9's thread-safe `posix_memalign`/`free`
via `#[global_allocator]`, printing `sum of squares 1..8 = 204` on the VM. Driver:
[`host/rust9`](host/rust9). Panic strategy is `abort` (no unwinder needed yet).

What's missing is **`std`** — and `std` is not distributed for `x86_64-unknown-none`
(it's `os = "none"`, bare metal). Getting `std` needs a target that carries
`os = "plan9"`, which is M2.

---

## 2. Milestones

Ordered. Each is a hard gate for the next.

### M2 — Custom `x86_64-unknown-plan9` target + `-Zbuild-std core,alloc`

**Deliverable:** `x86_64-unknown-plan9.json` in-tree, plus a `rust9-ld` linker
wrapper, such that `cargo +nightly build -Zbuild-std=core,alloc --target
x86_64-unknown-plan9.json` reproduces the M1 result through cargo instead of the
hand-rolled bash. This is the target-spec work only — no `std` yet — and it's the
prerequisite that makes `-Zbuild-std=std` even *attempt* to compile in M3.

Concretely:
- Derive the JSON from `x86_64-unknown-none` (see §4). The load-bearing changes are
  `os = "plan9"`, empty `families` (**not** `unix`), the SSE flip
  (`+sse,+sse2,-soft-float`) with `abi` cleared to hard-float SysV, `relocation-model
  = static`, `code-model = small`, `has-thread-local = false`, `panic-strategy = abort`.
- `rust9-ld`: a ~20-line wrapper on `PATH` that forwards rustc's `ld.lld` args, adds
  `-static -nostdlib -T .../plan9.ld` + the `--start-group` cc9 archives, then runs
  `elf2aout` to the `-o` path. Moves the current bash logic out of `host/rust9` and
  into the target's `linker` field.

**Effort:** ~1–2 days. Fork-and-patch is not needed *yet* — a bare JSON + build-std
compiles `core`/`alloc` for the new `cfg(target_os = "plan9")` immediately. Nightly
+ `rust-src` become mandatory here.

### M3 — Minimum-viable `std` over the cc9 runtime

**Deliverable:** a forked rust-lang/rust with a fresh `std::sys::pal::plan9` (**do
not** adapt the unix pal — see §3 verdict) wired to cc9's C ABI via `extern "C"`,
buildable with `-Zbuild-std=core,alloc,std,panic_abort`, that lights up:

- `println!` / `eprintln!` (stdio over fds 0/1/2),
- `File` + `fs::read_dir` (cc9's POSIX-over-9P slice → `Metadata`/`FileType`/`Permissions`),
- `env::var`/`args` (env over the `/env` filesystem; argv captured at init),
- `thread` + `Mutex`/`Condvar`/`RwLock` + channels (cc9 pthreads over `rfork`+semaphores),
- `Instant`/`SystemTime` (`/dev/bintime`), `HashMap` (seed from `/dev/random`).

`net`, `process`, `anonymous_pipe`, and `backtrace` are **honestly stubbed** to the
`unsupported` skeleton (every op returns `Unsupported`). Gate test: a program that
`format!`s, opens a file, walks a directory, reads `$home`, and spawns a thread that
sends on a channel — compiled via build-std, run on the box.

**Effort:** ~1–2 weeks. Of the whole `std` surface, only **four** pieces are
genuinely new Rust — `env` over `/env`, argv capture, the Plan 9 `Dir`/mode →
`Metadata` mapping, and TLS-key wiring. Everything else is thin `extern "C"` glue over
cc9 or near-verbatim reuse of the unix `os_str`/`path` modules, because **cc9 already
carries the POSIX-over-9P weight**. Zero cc9 runtime changes required for the MVP.

### M4 — cargo / build-std dev loop

**Deliverable:** a `.cargo/config.toml` + committed `x86_64-unknown-plan9.json` +
`rust9-ld` on `PATH` so that a normal Cargo project builds for 9front with plain
`cargo build`, and `host/rust9` grows a `cargo` subcommand that builds the workspace
staticlib/bin, runs `elf2aout`, and delivers it. Registers `RUST_TARGET_PATH`, pins
`build-std = ["core","alloc","std","panic_abort"]` and
`build-std-features = ["panic_immediate_abort"]`, and documents the toolchain pin
(nightly + `rust-src`).

**Effort:** ~2–3 days. Mostly config + wrapper glue; the risk is dependency crates
that assume `cfg(unix)` and won't compile (mitigated by the fact that our target sets
no `unix` family — deps that need it fail loud and early rather than miscompiling).

### M5 — Flagship program

**Deliverable:** one real, useful Rust program running on 9front, exercising the MVP
`std` surface (fs + args + env + threads + Unicode + regex), delivered and demoed on
the box. Recommended: **a pure-Rust CLI (`ripgrep`- or `fd`-class)** — see §5. Explicitly
**not** Alacritty for the first flagship.

**Effort:** the "hello, useful" version (search/walk a tree, no sockets/subprocess) is
days-to-weeks on top of M3; each dependency that reaches for `net`/`process`/mmap is a
mini-project of its own and should be triaged before adoption.

**Later / out of scope for the CLI path** (named so they're not forgotten):
`panic = "unwind"` (point Rust's `eh_personality` at cc9's DWARF `_Unwind_*` — the C++
runtime already proves the unwinder works), a real `sys::net` over the `/net`
filesystem (`dial` via `/net/tcp`, resolve via `/net/cs`), a real `sys::process` over
`rfork(RFPROC|RFFDG|RFENVG)`+`exec`, and `backtrace` symbolization from the a.out's own
DWARF. All are substantial standalone modules, none are on the CLI-MVP path.

---

## 3. `std`-port surface

Fresh `std::sys::pal::plan9`, calling cc9's C ABI directly via `extern "C"` — **no
`libc` crate**. "MVP" = required to compile+run a `ripgrep`/`fd`-class CLI.

| Module | MVP | What cc9 provides | Gap / work | Difficulty |
|---|:---:|---|---|---|
| `alloc` | ✓ | `malloc`/`free`/`posix_memalign`, `memset`/`memcpy` | None of substance — already wired in M1's `alloc.rs`; move glue into `sys::alloc` | trivial |
| `stdio` | ✓ | fds 0/1/2, raw read/write | ~30 lines: `Stdin`/`Stdout`/`Stderr` over cc9 read/write; `is_terminal` via `fd2path==/dev/cons` (optional) | trivial |
| `os_str` + `path` | ✓ | (pure Rust) | Reuse the unix `Bytes`/`'/'`-sep modules almost verbatim — Plan 9 paths are UTF-8, `/`-separated, prefix-free | trivial |
| `time` | ✓ | `/dev/bintime` (ns monotonic + wall) | Read `/dev/bintime` for `Instant` + `SystemTime`; map to `Duration` | trivial |
| `random` | ✓ | `/dev/random` (plain file) | Read into the `HashMap` seed (~10 lines); required in practice | trivial |
| `panic`/`unwind` | ✓ | DWARF EH + unwinder (proven by cc9 C++) | MVP: `panic = abort` (unchanged from today). `unwind` is a later stretch | trivial |
| `fs` | ✓ | full POSIX-over-9P: open/create/read/write/lseek/stat/fstat/mkdir/remove/rename/getcwd/chdir/readdir | **Real work:** map Plan 9 `Dir`/mode bits (`DMDIR`, rwx triads, mtime) → `FileType`/`Permissions`/`FileTimes`; accept no symlink/hardlink | moderate |
| `thread` | ✓ | pthreads over `rfork(RFMEM)`+semaphores; `/dev/bintime` | `Thread::new` over `pthread_create`; `sleep`; `available_parallelism` from `/dev/sysstat` (else 1). No mmap guard-page stack-overflow detection (omit) | moderate |
| `thread_local` (TLS) | ✓ | pthread key-style TLS | Use the key-based `Key` path with dtors; skip native `#[thread_local]` (no a.out TLS segment) | moderate |
| `sync` / locks | ✓ | pthread mutex/condvar/once | No futex → select std's pthread-backed non-futex `sys::sync` impls (already exist). Optional later: futex shim over `semacquire`/`rendezvous` | moderate |
| `env` | ✓ | (raw fs slice) | **NET-NEW ~60 lines:** no envp on the stack — the environment *is* `/env` (one file per var). `var`=read `/env/NAME`, `set_var`=write, `vars`=readdir `/env` | moderate |
| `args` | ✓ | crt0 passes `(argc, argv)` into `main` | **NET-NEW tiny:** capture argv into a static at init (no `/proc/self/cmdline` fallback exists) | moderate |
| `target` + build harness | ✓ | (host-side; ELF→a.out pipeline already exists) | Fork rust-lang/rust, add `plan9` arm to `cfg_select!` in `sys/pal/mod.rs`, ship the target JSON. Same model as zig9/python9 | moderate |
| `net` | — | **nothing** (no BSD sockets, no poll/epoll/kqueue) | **DEFER:** stub `sys::net` → `unsupported`. Real backend = a large custom module over `/net` (`/net/tcp`, `/net/cs`) | hard |
| `process` + `pipe` | — | **nothing** | **DEFER:** stub → `unsupported`. Real impl = `rfork(RFPROC\|RFFDG\|RFENVG)`+`exec` + wait-status parsing + Plan 9 `pipe()` | hard |
| `backtrace` | — | DWARF unwind exists; **no** `dl_iterate_phdr`, a.out ≠ ELF | **DEFER:** empty backtrace. Real symbolization needs a Plan 9 module-lister feeding the a.out's DWARF to gimli | hard |

**Verdict (why a fresh pal, not the unix pal):** the unix pal is a trap — it
hard-depends on the `libc` crate (no Plan 9 module exists; adding one upstream is
itself a blocker), assumes errno-in-TLS, POSIX signals/`sigaction`, fork/exec,
mmap-backed stacks + guard pages, and `O_CLOEXEC`; worse, setting
`target_family = "unix"` mis-signals the entire crate ecosystem (deps then assume BSD
sockets, fork, `/proc`). The decisive twist: **cc9 already exposes POSIX-*shaped* C
wrappers**, so a self-contained plan9 pal modeled after the `hermit`/`uefi`/`xous`
backends — starting from the `unsupported` skeleton and calling cc9 via `extern "C"` —
is *thin*: most feature modules are 10–30 lines. The modern feature-first `sys`
restructuring (rust-lang/rust #117276) shrinks the required pal surface further, since
locks/path/os_str/random-mixing now live in platform-agnostic `sys::<feature>` modules.

---

## 4. Custom target spec notes (`x86_64-unknown-plan9.json`)

Derive from stock `x86_64-unknown-none` (dump the baseline with
`rustc +nightly -Z unstable-options --target x86_64-unknown-none
--print target-spec-json`), then apply the overrides. Load-bearing fields:

**Identity / OS**
- `"os": "plan9"` — the whole point; the `cfg(target_os = "plan9")` `std::sys` keys off.
- `"families": []` — **not** `"unix"`. Plan 9 isn't POSIX; cc9 fakes only a POSIX
  *slice*. Claiming `unix` pulls `sys::unix` + the `libc` crate wholesale and mis-keys
  hundreds of cfgs.
- `"dynamic-linking": false`, `"executables": true` — a.out is static-only; no `dlopen`.
- `"llvm-target": "x86_64-unknown-none-elf"` — LLVM has **no** Plan 9 backend; keep the
  freestanding-ELF triple that `elf2aout` consumes. The Plan-9-ness lives in the linker
  wrapper, not codegen. Copy `data-layout` **verbatim** from `x86_64-unknown-none` (must
  byte-match what LLVM derives or rustc aborts).

**The SSE flip (the reason a custom spec is needed at all)**
- `"features": "+sse,+sse2,-soft-float"` — 9front amd64 *has* SSE; `unknown-none` ships
  `+soft-float` with SSE off. Flip it or you get soft-float codegen and no XMM.
- `"abi": ""` — **critical.** `unknown-none` sets `abi = "softfloat"` (floats in GPRs).
  cc9's runtime is host-clang-compiled with normal hard-float SysV (f32/f64 in xmm0…).
  Leaving `softfloat` set **silently corrupts every `extern "C"` call that passes/returns
  a float/double** (openlibm, `printf %f`). Clearing `abi` restores hard-float SysV; it
  must agree with `features`.

**Codegen / memory model** (match the fixed-address a.out, not a PIE kernel)
- `"relocation-model": "static"`, `"position-independent-executables": false`,
  `"static-position-independent-executables": false` — `plan9.ld` + `elf2aout` produce a
  fixed-load-address image, not PIE.
- `"code-model": "small"` — Plan 9 amd64 user text loads low (`UTZERO 0x200000`); the
  `kernel` code model assumes high-half addresses and mis-relocates.
- `"disable-redzone": true` — Plan 9 delivers notes by pushing a `Ureg` near SP; a red
  zone is unsafe. Cheap insurance.
- `"has-thread-local": false` — `#[thread_local]` wants ELF TLS, which a.out has no
  segment for. Forces std onto key-based TLS (`pthread_getspecific`), which cc9 provides.
- `"panic-strategy": "abort"` — phase 1, matches today. Phase 2: `"unwind"` once std
  links cc9's DWARF `_Unwind_*` + a Rust `eh_personality`.

**Linker** (wrapper driving `ld.lld` + `plan9.ld` + `elf2aout`)
- `"linker": "rust9-ld"`, `"linker-flavor": "ld.lld"`, `"link-self-contained": "no"`,
  `crt-objects-fallback = false`.
- `pre-link-args["ld.lld"] = ["-static","-nostdlib","-T",".../cc9/test/plan9.ld"]`
- `post-link-args["ld.lld"] = ["--start-group",".../libcc9cxx.a",".../libcc9m.a","--end-group"]`
  — same `--start-group` the current bash wrapper uses (Rust ↔ cc9 are mutually
  referential: Rust needs `malloc`/`memcpy`, crt0 needs `main`).

**build-std invocation (nightly-only)**
```
rustup toolchain install nightly
rustup component add rust-src --toolchain nightly
cargo +nightly build \
  -Z build-std=core,alloc,std,panic_abort \
  -Z build-std-features=panic_immediate_abort \
  --target /abs/x86_64-unknown-plan9.json
```
- `panic_abort` **must** be in the build-std list whenever `panic-strategy = abort`, or
  std won't link its panic runtime.
- **Do NOT** enable `compiler-builtins-mem` — cc9 already exports
  `memcpy`/`memmove`/`memset`/`memcmp`/`str*`; letting `compiler_builtins` also define
  them causes duplicate-symbol clashes. Let them resolve from `libcc9cxx.a`.

**Data-model note** (ties to the known Plan 9 bug class): the "amd64 is not LP64"
gotcha is about *native kencc* (`long = 32`). rust9's C-ABI peer is **cc9** =
host-clang → LP64 (`long = 64`), so Rust's default `core::ffi::c_long = i64` is already
correct here — **do not** narrow it. Py_ssize_t-style 4-vs-8 mismatches come from kencc,
not from this pipeline.

**Tradeoff vs `x86_64-unknown-none`:** keep `unknown-none` for the "does Rust run at
all" core+alloc demo (stable rustc, `rustup target add`, no build-std). Move to the
custom JSON only when you want `std`, accepting: nightly + build-std become mandatory,
you compile core/alloc/std from source every clean build, you own the JSON (it can drift
against rustc's `TargetOptions` schema on nightly bumps), and — the biggest hidden cost
— **`std` doesn't build until `std::sys::plan9` exists**. The JSON is the prerequisite;
the pal port (M3) is the actual body of "real std support."

---

## 5. Flagship triage: Alacritty is the wrong first flagship

**Honest verdict: no.** Alacritty was read verbatim at current master
(alacritty 0.18.0-dev / alacritty_terminal 0.26.1-dev, edition 2024, MSRV 1.85). The
terminal *model* — `vte` (the actual ANSI/escape parser), the grid, config
(serde/toml/json/yaml, log, bitflags, clap, unicode-width, regex-automata, base64,
`ahash` with `no-rng`) — is pure `core`+`alloc` and lands on rust9 essentially free.
But **everything that touches the OS is a wall**, and there are five *confirmed,
could-not-refute* walls. All the porting cost is concentrated in windowing, the pty,
fonts, and the GL bridge — none of which the MVP `std` even provides, and each of which
is a project in itself.

### Confirmed blockers (verified, high confidence)

1. **`winit 0.30.9` — no Plan 9 windowing backend.** Backends are
   win32/appkit/uikit/x11/wayland/android/web/orbital only; Plan 9 appears nowhere, and
   no fork/shim/crate exists. Must author a new backend over `/dev/draw`+libdraw (window),
   `/dev/mouse` + `/dev/kbd`/`cons` (input), `/dev/wctl` (resize). Redox's Orbital backend
   is the correct precedent (non-Unix, filesystem/IPC-backed). **Highest-effort item;
   everything visual gates on it.**
2. **PTY/tty layer (`rustix-openpty 0.2` + `rustix 1.0` + `libc` ioctls).**
   `alacritty_terminal/src/tty/unix.rs` is intrinsically Unix-pty-shaped:
   `rustix_openpty::openpty` (`/dev/ptmx`+`grantpt`/`unlockpt`), `setsid` via `pre_exec`,
   `ioctl(TIOCSCTTY)`, `ioctl(TIOCSWINSZ)`, `fcntl` non-blocking. **Plan 9 has none of
   this** — the APE manual states outright "the concept of a controlling tty is foreign
   to Plan 9." Needs a ground-up rewrite where the emulator becomes a 9P file server
   exposing a per-window cons + `rfork`/`exec` (how rio/9term/drawterm actually work).
3. **`crossfont 0.8.1` — fonts via `freetype-rs` + `yeslogic-fontconfig-sys` (two C
   libs).** FreeType is portable C (compilable via cc9). **fontconfig is Plan-9-hostile**
   (XML config, on-disk caches, expat) and is a mandatory, non-feature-gated dep on the
   Unix path — must be stubbed/replaced (bundle TTFs or map to `/lib/font`).
4. **`glutin 0.32.2` (egl) + GL renderer.** Alacritty is *fundamentally* OpenGL (GLES2/GL
   3.3 via `gl_generator`) with **no software fallback** (maintainers call adding one "a
   nontrivial architecture redesign"). Needs the gl9/gl9egl Mesa-softpipe EGL seam, and is
   **doubly gated** — it consumes a `raw-window-handle`, which has no Plan 9 variant, so it
   can't attach until the winit backend exists.
5. **`polling 3.x` + `libc 0.2` — fd-readiness reactor.** No Plan 9 target; 9front has no
   epoll/kqueue/poll, and `libc` has no plan9 module at all (so every ioctl/`setsid`
   constant must be hand-supplied). Plan 9's idiom is a blocking-read-per-proc, which
   *mismatches* the readiness model — the reactor should be replaced (reader-proc-per-fd),
   not ported. `rustix`+`signal-hook` (also unix-only) compound this.

Smaller secondary shims (real but cheap): `copypasta` (no Plan 9 backend, but `/dev/snarf`
is ~30 lines), `signal-hook` (Unix signals → Plan 9 notes, which the runtime already
provides), `notify` (config live-reload — droppable/pollable), `tempfile` + `parking_lot`
(thin name-randomness / thread-park shims).

**Net:** Alacritty forces us to build a windowing backend, a pty subsystem, a font stack,
*and* a GL/EGL bridge — before its (excellent, free) terminal core does anything visible.
That's four hard subsystems, three of which (`winit`, tty, GL) don't exist on 9front in
any form and none of which the `ripgrep`-class MVP needs.

### Recommendation

**First flagship (M5): a pure-Rust CLI — `ripgrep` or `fd`.** It exercises exactly the
MVP `std` surface we're building (fs walking, args, env, Unicode, regex, threads/channels),
needs **zero** of the five blockers, and delivers immediate real value on a box that is
chronically short on good userland tooling.

| Candidate | Exercises | Blockers | Effort | Appropriateness |
|---|---|---|:---:|:---:|
| **ripgrep / fd** | fs, args, env, Unicode, regex, threads | none (may want `net=no`, `process` avoidable) | **days–weeks** on M3 | **best first flagship** |
| **Alacritty (as-is)** | terminal core (free) + winit + pty + fonts + GL | **all 5** | **months** | poor first pick |
| **Native-libdraw Rust terminal** | `std` + `vte` (reuse!) + `/dev/draw` directly | none of the 5 | **weeks–month+** | best *terminal* pick |

**If a terminal is the real want:** do **not** port winit+glutin+crossfont+PTY. Write a
small native 9front terminal in Rust that (a) reuses Alacritty's *free* half — `vte` for
ANSI parsing + a grid model, (b) draws to `/dev/draw`/libdraw directly (or via the gl9
window seam) instead of winit+GL, (c) reads input from `/dev/mouse` + `/dev/kbd`, and (d)
serves cons over 9P + `rfork`/`exec` for the shell — i.e., builds *with* Plan 9's grain
(the rio/9term model) instead of against it. That sidesteps all five blockers, reuses the
one genuinely valuable, genuinely portable piece of Alacritty (`vte`), and is a
weeks-scale project rather than a months-scale multi-subsystem port. Alacritty-the-hard-way
is the wrong shape of work for this platform; the native terminal is the same *goal* at a
fraction of the cost.
