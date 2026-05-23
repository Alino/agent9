---
source_url: https://github.com/oridb?tab=repositories
ingested: 2026-05-17
sha256: snapshot-only
---

# Ori Bernstein (oridb) — GitHub Repository Inventory

Plan 9 developer. Creator of Myrddin. Site: http://eigenstate.org/.
202 followers. 8.4k commits across repos.

## Top by stars (originals)

| Repo | Lang | Stars | Forks | License | Last activity |
|---|---|---|---|---|---|
| **mc** — Myrddin Compiler | C | 408 | 33 | MIT | 2022 (maintenance) |
| **git9** — Git for plan 9 | C | 208 | 8 | MIT | May 2024 |
| **mk** — Makefile templates | Makefile | 20 | 3 | MIT | — |
| **myrsite** — Myrddin website | CSS | 4 | 4 | — | — |

## Myrddin ecosystem

Core compiler `mc` plus the standard library + tooling, scattered as separate repos:

- **libthread** — thread library (Assembly, May 2016)
- **libregex** — regex library (Apr 2015)
- **libbio** — buffered IO (Feb 2015)
- **libcryptohash** — crypto hashes (Aug 2015)
- **libdate** — dates (Nov 2015)
- **libtermdraw** — terminal handling library for Myrddin (2 stars)
- **libwl** — Wayland client implementation in Myrddin (fork, Python build glue)
- **mbld** — build tool (older version; current mbld lives in mc/)
- **myrbuild** — older C rewrite of build system (Apr 2015)
- **muse** — `.use` file generator (in mc/)
- **mkchartab** — Unicode character table generator
- **sundown-myr** — Sundown markdown (C, Sep 2014)
- **ctags-myr** — ctags fork for Myrddin (Aug 2014)
- **cbind-example** — example C binding for Myrddin
- **homebrew-myrddin** — brew tap to install Myrddin on macOS
- **myrbox** — (C)

## Plan 9 / Unix tools

- **git9** — Plan 9 git client (most active; daily-driver for Ori)
- **plan9port** — fork of 9fans/plan9port
- **rc** — fork of qwx9/rc shell tests (Jun 2024)
- **awk** — fork of onetrueawk/awk
- **libproto9** — fork from andrewchambers

## Other originals

- **j** — "Journal programs so small they only need one letter" (Shell, WTFPL)
- **hairless** — parser generator
- **contbuild** — continuous build (2 stars)
- **pirateonion** — "A program to arrr-chive your data" (MIT)
- **mparse** — (2 stars, MIT)
- **pdffs-ocaml** — "another one"

## Notable forks

- **go** — golang/go fork (BSD)
- **discount** — Orc/discount markdown
- **btrdb** — Berkeley Tree Database Go bindings
- **ice** — pion/ice Go port
- **minsig** — saljam/webwormhole fork (Go)
- **libtrue** — "You can't handle the truth" (C, BSD 2-Clause)
- **wycheproof** — crypto attack tests (Java, Apache)
- **yasm** — yasm/yasm fork
- **qc** — andrewchambers/qc (Quick C)
- **mlisp**, **myros**, **crypto** — andrewchambers forks

## Themes

1. **Myrddin is the throughline.** Almost everything links back to it.
2. **Plan 9 ecosystem maintainer.** git9 is the de-facto Plan 9 git;
   his plan9port fork ships fixes upstream picks up.
3. **Minimalist aesthetic.** "Solid Engineering" tagline + Tacoma Narrows
   bridge image on mc; `j` (one-letter journal); `pirateonion`.
4. **Languages used:** C, Assembly, Shell. No Rust, no Python in
   production code.
