---
title: pac9 — package manager for 9front
created: 2026-07-01
updated: 2026-07-01
type: concept
tags: [plan9, rc, toolchain, done]
---

# pac9 — package manager for 9front

`pac9 install <name-or-git-url>` — one line to fetch, build, and install a
package on a running 9front box. Source in `pac9/`.

## Why it's tiny

9front already standardizes the hard parts, so pac9 doesn't reinvent them:

- [[git9]] (`git/clone`) fetches any repo — no download code to write.
- The `mkfile` + `</sys/src/cmd/mkone` convention gives every well-formed
  package a `mk install` target that drops its binary in `/$objtype/bin`.

So "install any git repo" is essentially `git/clone URL; cd; mk install`. pac9
is the thin rc wrapper that adds a name→url registry, build-step detection, and
an install manifest. It mirrors the manual flow proven in [[netsurf-install]]
(`git/clone` → `fetch clone http` → `mk` → `mk install`).

## Files

| File | Purpose |
|---|---|
| `pac9/pac9` | the tool (rc) — `install` / `list` / `uninstall` |
| `pac9/registry` | curated `name<TAB>url<TAB>subdir<TAB>recipe` table |
| `pac9/install.rc` | bootstrap: cp into `/rc/bin` + `/sys/lib/pac9` |
| `pac9/test.rc` | no-git self-check (registry parse + recipe install + manifest) |

## Runtime state

```
/rc/bin/pac9                the script (rc scripts live here)
/sys/lib/pac9/registry      curated name→url table
/sys/lib/pac9/installed      manifest:  name url srcdir  (one line per pkg)
$home/src/pac9/<name>/       cached clone, reused on re-install
```

## install flow

`install` takes any number of packages, each handled in its own subshell so one
failure doesn't sink the batch. For each:

1. **Resolve** — registry hit gives url/subdir/recipe/deps; otherwise the arg is
   a git URL (`name` = basename minus `.git`).
2. **Dependencies** — install any names in the registry entry's `deps` column
   (5th, space-separated) that aren't already in the manifest. Each dep recurses
   through `install` in a subshell, which also keeps the recursion from
   clobbering the parent's resolve() globals. So `pac9 install pi9` pulls in
   vts + vtwin.
3. **Fetch** — `git/clone` (or `git/pull`) into `$home/src/pac9/<name>`. https
   needs `webfs` running (it is, on the image — see [[build-toolchain]]).
4. **Build** — custom recipe if given, else `mk install`, else `mk`, else
   `build.rc`, else stop and print the source path.
5. **Record** — append `name url srcdir` to the manifest (dedup by name).

## Two package kinds

- **Source** (`url` is a git URL) — cloned and built. Most things, including the
  repo's own `src/*` components via a `subdir` field (`src/mxio`, `src/vts`, …).
- **Prebuilt** (`url` is `-`) — no repo; the recipe fetches a tarball and
  unpacks it. This is how **python9 / node9 / zig9 / pi9** install: their
  vendored upstream (or cross-compiled Go, for pi9) isn't cloneable-and-buildable
  on the box, so they ship as built artifacts unpacked at `/` (the same
  `hget | gunzip | tar x` flow already used to place them on the image).
  Uninstall for these is best-effort (removes the `/$objtype/bin` binary, not
  the `/sys/lib` tree).

## Registry format

Tab-separated, `#` comments. `subdir` `.` = repo root; `recipe` `-` = default
detection. You only need an entry for a short name or a custom build — unknown
names fall through to being treated as a git URL.

## Status

Implemented 2026-07-01. Placeholder tarball URLs for the cross-ports point at
`github.com/Alino/agent9` releases — swap in real asset tags once published.

## See also

- [[git9]] — the native git client pac9 clones with
- [[netsurf-install]] — the manual flow pac9 automates
- [[build-toolchain]] — hget / mk / rc mechanics pac9 relies on
