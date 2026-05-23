---
source_url: https://myrlang.org/
ingested: 2026-05-17
sha256: snapshot-only
---

# Myrddin Programming Language

Designed by Ori Bernstein (oridb). MIT-licensed.

## One-liner

"Aims to fit into a similar niche as C, but with fewer bullets in your feet.
Does not aim to explore the forefront of type theory or compiler technology.
Satisfied to be a practical, small language."

## Feature surface

- Strong static type checking
- Whole-program type inference
- Generics (`@t` type parameters, optional trait bounds)
- Algebraic data types + pattern matching (closer to ML unions than C unions)
- Closures
- Traits
- Almost no runtime
- Self-contained toolchain
- No implicit type conversions

## Supported platforms

x86-64 only. Runs on:

- Linux, OSX, FreeBSD, OpenBSD
- **Plan 9 (9front)** ← the relevant bit
- NetBSD (preliminary, no libthread)

## Toolchain

- `6m` — compiler (the only one — name follows Plan 9 `?c` convention)
- `mbld` — build tool. Wraps the whole pipeline: deps, build order, link,
  install. No Makefiles.
- `muse` — `.use` file generator (Myrddin's answer to header files).
  Auto-invoked by `mbld`.

## Hello World

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

## Type system primitives

| Type | Notes |
|---|---|
| `void`, `bool`, `char` (char = Unicode codepoint) | |
| `int8/16/32/64`, `int` | int ≥ 32 bits |
| `uint8/16/32/64`, `uint` | |
| `flt32`, `flt64` | |
| `type#` | pointer to single element, no arithmetic |
| `type[:]` | slice (data + length) |
| `type[SIZE]` | fixed array, size is part of type |

## Packages

- No headers. `pkg` block declares exports.
- `use std` — search path
- `use "filename"` — local

## Pattern matching

```myr
match (i % 3, i % 5)
| (0, 0): std.put("fizzbuzz\n")
| (0, _): std.put("fizz\n")
| (_, 0): std.put("buzz\n")
| _:
;;
```

Backtick syntax for ADT tags: `` `Some 123 ``, `` `None ``,
`` `std.Ok r ``, `` `std.Err m ``.

## Standard library highlights

`std`, `regex`, `bio` (buffered IO), `cryptohash`, `date`, `thread`.
Most ship as separate repos in github.com/oridb/.

## Status

- 408 stars, 21 contributors, 5198 commits.
- Last activity on the compiler: May 2022 (maintenance mode).
- Online playground: myrlang.org/playground
- API reference: myrlang.org/doc

## Sources

- Eigenstate (Ori's main mirror): http://eigenstate.org/
- 0intro interview (audio + transcript): https://0intro.dev/ori/
- First impressions article: https://jakob.space/blog/first-impressions-of-the-myrddin-programming-language.html
