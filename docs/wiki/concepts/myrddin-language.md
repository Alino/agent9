---
title: Myrddin Language
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [plan9, toolchain]
sources: [raw/articles/myrddin-language-overview-2026-05.md, raw/articles/oridb-repos-overview-2026-05.md]
---

# Myrddin Language

A small, practical systems language by Ori Bernstein ([[oridb-ecosystem]]).
Runs natively on **9front amd64**.

## Why we care

It's the only modern statically-typed language with a working Plan 9
target. Go's Plan 9 port works (we use it for [[pi9-architecture]]) but
Go is a runtime-heavy language with a GC. Myrddin is C-shaped — no GC,
explicit lifetimes, no implicit conversions — but with ML-style pattern
matching, ADTs, generics, and traits on top.

If you ever want a `mxio` rewrite that isn't C and isn't Go, Myrddin is
the candidate. Probably never — but worth knowing exists.

## One-paragraph summary

Myrddin compiles via `6m` (the only backend — name follows Plan 9 `?c`
convention). Builds with `mbld`, which replaces Makefiles by deriving deps
from `use` statements. No headers — package metadata lives in
auto-generated `.use` files. Standard library is small, in `lib/std`.
Targets amd64 only.

## Feature surface

- Strong static types, **whole-program type inference**
- Generics (`@t`, optionally bounded by traits)
- **Algebraic data types + pattern matching** (closer to ML than C)
- Closures, traits
- Slices (`type[:]`) as a first-class fat-pointer type
- No implicit conversions
- Almost no runtime, self-contained toolchain

## Hello, world

```myr
use std

const main = {args : byte[:][:]
        std.put("Hello-世界\n")
        for a : args
                std.put("arg = {}\n", a)
        ;;
}
```

Build: `mbld -b hello hello.myr`

## Pattern matching idiom

The thing it does that C cannot:

```myr
match regex.compile(".*")
| `std.Ok r:    re = r
| `std.Err m:   std.fatal("couldn't compile regex: {}\n", m)
;;
```

Backtick tags are ADT constructors. `std.Ok`/`std.Err` is Myrddin's
Result-style sum type. Compiler enforces exhaustiveness.

## Toolchain

| Binary | Role |
|---|---|
| `6m` | the compiler |
| `mbld` | build tool — wraps deps + link + install |
| `muse` | `.use` file generator (auto-invoked by mbld) |

## Install on 9front

```rc
% cd /tmp
% hget https://github.com/oridb/mc/archive/HEAD.tar.gz | tar xvz
% cd mc-*
% ./configure
% mk all
% mk install
```

(Or use [[git9]] to clone.)

## Status

- 408 stars, 21 contributors, 5198 commits.
- Last activity on the compiler: May 2022. **Maintenance mode.**
- No active language evolution. The spec is what it is.
- Used in production by Ori for personal projects + some embedded work.

## When NOT to reach for it

- For our four components ([[mxio-design]], xena-panel, launcher, xfiles)
  — stick with K&R C. The whole project standardises on it. Mixing
  Myrddin would fragment the codebase for no win.
- For pi9 — Go gives us bubbletea, lipgloss, charmbracelet, OpenRouter
  clients, the whole agent ecosystem. Myrddin starts from zero.
- For browser engine work ([[browser-webview-plan9]]) — same reason, no
  ecosystem.

## When it might fit

- A hobby project that wants strong types + pattern matching + Plan 9
  native + zero runtime.
- A small tool where C feels heavy (parser-heavy code, ADT-shaped data).

## Sources

- https://myrlang.org/
- https://github.com/oridb/mc
- 0intro interview with Ori (audio): https://0intro.dev/ori/
- Eigenstate: http://eigenstate.org/

## See also

- [[oridb-ecosystem]] — the rest of Ori's Plan-9-adjacent work
- [[git9]] — Ori's other big Plan 9 contribution
