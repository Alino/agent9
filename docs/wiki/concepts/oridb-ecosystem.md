---
title: oridb (Ori Bernstein) Ecosystem
created: 2026-05-17
updated: 2026-05-17
type: reference
tags: [plan9, toolchain]
sources: [raw/articles/oridb-repos-overview-2026-05.md, raw/articles/myrddin-language-overview-2026-05.md, raw/articles/git9-readme-2026-05.md]
---

# Ori Bernstein (oridb) Ecosystem

Independent Plan 9 contributor. Creator of [[myrddin-language|Myrddin]].
Author of [[git9]]. Site: http://eigenstate.org/. Active mostly via
9front, github mirror used for visibility.

Complement to [[9fans-ecosystem]] — that page covers the official 9fans
org (plan9port, drawterm, etc.). This one covers Ori's parallel universe.

## Repositories by relevance to plan9-winxp

| Tier | Repo | Why we care |
|---|---|---|
| **A** | [oridb/git9](https://github.com/oridb/git9) | Daily-driver git in our 9front VM. See [[git9]]. |
| **A** | [oridb/mc](https://github.com/oridb/mc) | Myrddin compiler — the only modern typed language with a Plan 9 target. See [[myrddin-language]]. |
| **B** | [oridb/plan9port](https://github.com/oridb/plan9port) | Ori's fork of [[9fans-ecosystem|9fans/plan9port]]. Sometimes has fixes the main tree hasn't picked up yet. Read-only reference. |
| **B** | [oridb/rc](https://github.com/oridb/rc) | Fork of qwx9/rc shell tests. Useful if we hit shell edge cases. |
| **C** | [oridb/awk](https://github.com/oridb/awk) | Fork of one-true-awk. Probably already in 9front; not a reason to install. |
| **C** | [oridb/libproto9](https://github.com/oridb/libproto9) | 9P library fork. We use Plan 9's native lib9p; this is reference only. |

## Repositories NOT relevant

- All Myrddin lib repos (libthread, libregex, libbio, libcryptohash, libdate,
  libtermdraw, libwl, mbld, myrbuild, sundown-myr, ctags-myr, mkchartab,
  cbind-example, homebrew-myrddin, myrbox) — only matter if we write
  Myrddin code. Not in scope for the four components.
- `j` (one-letter journal), `hairless`, `contbuild`, `pirateonion`,
  `mparse`, `pdffs-ocaml` — personal projects, no Plan 9 angle for us.
- Forks of golang/go, btrdb, ice, minsig, fboss, fbthrift, druntime,
  PhotonOS, nope.c, wycheproof, yasm, MLX90621_Arduino_Processing — not
  Plan 9, not our domain.

## Why git9 is on github at all

Same as Ori's plan9port: **github as upstream, 9front as downstream
mirror**. Commits land in oridb/git9, then someone (Ori, sirjofri, qwx)
runs a sync into the 9front tree. Last sync commit message in git9 is
literally "git: sync with 9front" (2024-05-27).

This is the working model for "I want my Plan 9 code visible to non-Plan 9
people without forcing them into hg/mercurial-on-9front workflows."

## Connection to Plan 9 community

Co-contributors across his repos overlap heavily with 9front maintainers:

- **okvik** — frequent git9 contributor, 9front committer
- **michaelforney** — chimera-linux author, plan9port maintainer
- **mischief** — long-time Plan 9 contributor
- **fhs** — acme-lsp maintainer (lives in [[9fans-ecosystem]])
- **andrewchambers** — author of qc (Quick C), several of Ori's forks
  start from his repos

Knowing who's in this network helps when reading commit logs or
mailing-list threads.

## How we'd actually consume this work

1. **git9** — install it inside the 9front VM, use it for source control.
   Stop using `hget tarball` after first bootstrap.
2. **Myrddin** — note that it exists. Don't reach for it unless we hit a
   specific need that C and Go don't cover.
3. **Ori's plan9port fork** — check it when the main plan9port misbehaves
   on macOS; sometimes the fix lives here first.

## Sources

- https://github.com/oridb?tab=repositories
- http://eigenstate.org/
- 0intro podcast with Ori: https://0intro.dev/ori/

## See also

- [[9fans-ecosystem]] — the official 9fans org
- [[myrddin-language]]
- [[git9]]
- [[9front-release-status]]
