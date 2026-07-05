# minirustc — a Rust compiler that runs on 9front

A small **native Rust compiler that runs directly on 9front** and compiles real
Rust programs to native Plan 9 executables — the whole pipeline (read source →
parse → codegen → assemble/link → run) executes on the box.

```
term% minirustc fib.rs
minirustc: compiled 3 fn(s) -> /tmp/minirustc_out.c (433 bytes of C)
minirustc: 6c minirustc_out.c
minirustc: 6l -o minirustc_out minirustc_out.6
minirustc: built /tmp/minirustc_out -- running it:
----
0 1 1 2 3 5 8 13 21 34 55 89 144
3628800
----
```

## How it works

minirustc is itself a Rust program, **cross-compiled for 9front with
[`rust9`](../)**. On the box it:
1. reads the `.rs` source (`std::fs`),
2. lexes + parses a subset of Rust (recursive-descent),
3. generates Plan 9 C (`vlong`, `print`, `exits`),
4. drives the on-box **kencc** toolchain — `6c` then `6l` — via
   **`std::process::Command`** (the process/exec support this port added),
5. runs the resulting native a.out.

So it exercises the full new std surface: `fs`, `process`/`exec`, `args`, and I/O.

## What it is — and isn't

It **is** a genuine Rust compiler running on Plan 9: it compiles a working
subset — `fn` with `i64` params + recursion, `let`/`let mut`, assignment,
`if`/`else`, `while`, the arithmetic/comparison/logical operators, function
calls, and `println!("{}", e)` / `print(e)`. `examples/{fib,primes}.rs` compile
and run correctly on 9front.

It is **not** `rustc`. Full rustc needs LLVM (C++) and a from-scratch compiler
bootstrap cross-compiled for plan9 — a multi-week port (LLVM is reachable, since
cc9 already runs clang/lld on the box; see the rust9 ROADMAP). minirustc is the
honest, achievable "Rust compiler on 9front": small, real, and self-hosted on
the platform. The complement is rust9 itself, which cross-compiles *all* of Rust
(via real rustc) **to** 9front.

## Use

```
# cross-build the compiler for 9front (needs the rust9 toolchain set up)
cargo +nightly build --release          # -> target/x86_64-unknown-plan9/release/minirustc
# then on 9front:  minirustc yourprog.rs
```
