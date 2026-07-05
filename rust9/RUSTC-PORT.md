# Porting rustc to run natively on 9front

Goal: a **faithful rustc** — the real compiler, not a subset — running directly on
9front. This is a large, multi-session engineering effort; this file tracks the
path, what's done, and what remains.

## The path: cranelift, not LLVM

rustc's default codegen is **libLLVM (C++)**. Two facts decide the route:
- cc9 already runs clang/lld on 9front, so LLVM *can* run on the box — **but cc9's
  LLVM is 22.1.8 and rustc 1.98 wants a different LLVM**, and `rustc_llvm`'s FFI is
  version-locked. Building rustc's *specific* patched LLVM for 9front is a port of
  its own.
- **`rustc_codegen_cranelift` (cg_clif)** is a pure-Rust codegen backend — no C++,
  no LLVM. It's in the tree (`compiler/rustc_codegen_cranelift`).

So the faithful port targets **cranelift**: no LLVM version-matching, everything is
Rust that cross-compiles with the rust9 toolchain. cranelift emits object files;
the on-box lld (from cc9) links them → a.out.

## Done (this session)

- **All std prerequisites rustc needs are real on 9front** (this is the big one):
  `fs`, `std::process` (rfork+exec) + pipes + `Command::output()`, real threads +
  `Mutex`/`Condvar`/`RwLock`, `panic = unwind` (cc9 DWARF), env/args/time. rustc does
  **not** use `net`; `mmap` (rmeta) has a read() fallback; `backtrace` can stub. So
  the std surface is largely covered.
- **Full compiler source** acquired at the exact nightly commit (`f46ec5218`,
  1.98.0-nightly) → `~/Projects/rust-src-full` (contains `compiler/` + cg_clif +
  bootstrap, which rust-src alone does not).
- **`x86_64-unknown-plan9` is now a built-in target in the compiler**, not just a
  JSON spec: `Os::Plan9` added to the `Os` enum, `spec/base/plan9.rs`,
  `spec/targets/x86_64_unknown_plan9.rs`, registered in `supported_targets!`.
  (SSE on, hard-float, static reloc, small code-model, panic=unwind, `families=[]`,
  `host_tools=true`, linker=`rust9-ld`.)
- The 38-file plan9 std overlay is integrated into the checkout's `library/`.
- **CONFIRMED end-to-end (this session):** a full bootstrap `x.py build --stage 1
  library --target x86_64-unknown-plan9` succeeds — it builds a stage1 rustc from the
  modified source, builds the **cranelift** backend, and builds the **entire plan9 std
  sysroot** (libstd/libcore/liballoc/libtest/… under
  `stage1/lib/rustlib/x86_64-unknown-plan9/lib/`). That stage1 rustc then compiled a
  real `std` program for the **built-in** plan9 target (no JSON) which **ran on 9front**
  (`sum of squares 1..10 = 385`). Needed two small fixes: `deny-warnings=false` (a new
  tier-3 target has a benign `dylib` note) and `rust9-ld` stripping the `-flavor gnu`
  lld-driver directive the built-in `gnu-lld` flavor emits.

## Remaining (the multi-session mountain)

1. **Statically link cg_clif into rustc.** cg_clif normally loads as a dynamic
   codegen backend; Plan 9 has no `dlopen`, so it must be compiled in statically.
2. **Bootstrap cross-HOST build.** x.py readily cross-compiles std/tools *to* a
   target, but building the *compiler itself* for a brand-new tier-3 host (plan9)
   is not a supported path — it needs bootstrap work: build every `rustc_*` crate +
   all host dependencies for `x86_64-unknown-plan9`, using the rust9 std, and link a
   `rustc` a.out. Expect to surface dependency-level `cfg(unix)` assumptions.
3. **Fill any compiler-dep std gaps** the cross-build surfaces (jobserver over
   pipes — pipes are done; `memmap2` → read fallback; `backtrace` → stub).
4. **Run on 9front.** rustc on the box invokes cranelift (in-process) → objects →
   on-box lld → a.out. Prove it compiles a real Rust program natively.

## The host-build wall — measured, not guessed

`x.py build --stage 2 --host x86_64-unknown-plan9 compiler/rustc` reaches a concrete,
fundamental blocker:

```
error: cannot produce dylib for `rustc_driver` as the target
       `x86_64-unknown-plan9` does not support these crate types
```

rustc ships as a thin `rustc` binary + a `librustc_driver.dylib` (and loads its
codegen backend and proc-macros as **`.so`s** at runtime). **Plan 9 has no dynamic
linking**, so the whole compiler must link statically. Forcing `rustc_driver` to
`rlib` cascades into **229 errors** — every compiler crate *and* its host deps
(`objc2`/`block2` = macOS, `nix` = unix, `rustc_codegen_llvm`/`rustc_llvm`) demand
dylib format. So a static rustc for Plan 9 needs, in concert:
- the entire compiler crate graph built as rlib/static (not one crate — all of them),
- **cg_clif statically linked in** (no dynamic codegen backend; no dlopen),
- **proc-macros** handled without `.so` loading (a real design change),
- host-only deps (objc2/nix/llvm) excluded from the plan9 host build.

That is genuine multi-week+ compiler-architecture engineering, not a single session.

## Honest status

**Reached this session:** `x86_64-unknown-plan9` is a **first-class built-in rustc
target** — the compiler cross-compiles real `std` programs to it and they **run on
9front** (proven), with a full plan9 std sysroot built by the real bootstrap and the
**cranelift** (no-LLVM) path validated. The remaining piece — making rustc itself a
static binary for a dynamic-linking-free OS — is concretely identified and bounded
above. The in-tree target spec, the integrated std, and this measured blocker map are
the foundation to continue from.
