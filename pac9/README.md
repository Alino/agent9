# pac9 — a package manager for 9front

One line to install software on a running 9front box:

```
pac9 install netsurf              # curated short name
pac9 install https://host/repo    # any git repo
pac9 list
pac9 uninstall netsurf
```

## How it works

9front already does the hard parts: `git/clone` (git9) fetches any repo, and
the `mkfile` + `</sys/src/cmd/mkone` convention gives every well-formed package
a `mk install` target that drops its binary in `/$objtype/bin`. pac9 is the
thin wrapper on top — a name→url registry, build-step detection, and a manifest
of what's installed.

`pac9 install <arg>`:

1. **Resolve** — if `<arg>` is a name in `/sys/lib/pac9/registry`, use its
   url/subdir/recipe; otherwise treat `<arg>` as a git URL.
2. **Fetch** — `git/clone` into `$home/src/pac9/<name>` (or `git/pull` if
   already there). https URLs need `webfs` running (it is, on the agent9 image).
3. **Build** — a custom recipe if given, else `mk install` (when the mkfile has
   that target), else `mk`, else `build.rc`, else it stops and tells you where
   the source is.
4. **Record** — appends `name url srcdir` to `/sys/lib/pac9/installed`.

## Install pac9 itself

From this directory, on the 9front box:

```
rc install.rc      # cp pac9 -> /rc/bin, cp registry -> /sys/lib/pac9
```

## Registry

`/sys/lib/pac9/registry`, tab-separated: `name<TAB>url<TAB>subdir<TAB>recipe`.

- `subdir` `.` = repo root (use e.g. `src/vts` for a package inside a monorepo).
- `recipe` `-` = default detection; otherwise an rc command run in the package
  dir (e.g. netsurf's `fetch clone http; mk; mk install`).

You only need an entry for packages that want a short name or a custom build —
unknown names fall through to being treated as a git URL.

### Two kinds of package

- **Source** (`url` is a git URL) — cloned and built. This is most things.
- **Prebuilt** (`url` is `-`) — no repo; the recipe fetches a tarball and
  unpacks it. Used for the big cross-ports whose source isn't self-contained.

## python9 / node9 / zig9

These are large cross-ports (CPython, QuickJS+npm, Zig). Their vendored upstream
source is **not** in the repo and building it on a TCG VM takes 20–60 min, so
they ship as **prebuilt tarball packages**: `pac9 install python9` fetches the
built binary + libdir and unpacks it at `/` — the same `hget | gunzip | tar x`
flow used to place them on the image today. Point the placeholder tarball URLs
in `registry` at the real GitHub Release assets once published.

Uninstall for these is best-effort (removes the `/$objtype/bin` binary but not
the `/sys/lib` tree — those ports have no file manifest).

## Verify

`rc test.rc` builds a throwaway package end-to-end (clone → `mk install` →
manifest) and asserts the binary appears. Run it on the 9front box.
