# pac9 — a package manager for 9front

One line to install software on a running 9front box:

```
pac9 install netsurf              # curated short name
pac9 install vts vtwin            # several at once
pac9 install pi9                  # pulls in its deps (vts, vtwin) too
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

`install` takes any number of packages; each is handled independently, so one
bad build doesn't sink the rest of the batch. For each one:

1. **Resolve** — if the arg is a name in `/sys/lib/pac9/registry`, use its
   url/subdir/recipe/deps; otherwise treat it as a git URL.
2. **Dependencies** — install any packages named in the registry entry's `deps`
   column that aren't already in the manifest, then continue.
3. **Fetch** — `git/clone` into `$home/src/pac9/<name>` (or `git/pull` if
   already there). https URLs need `webfs` running (it is, on the agent9 image).
4. **Build** — a custom recipe if given, else `mk install` (when the mkfile has
   that target), else `mk`, else `build.rc`, else it stops and tells you where
   the source is.
5. **Record** — appends `name url srcdir` to `/sys/lib/pac9/installed`.

## Install pac9 itself

On a **stock 9front** (no image, no clone) — fetch the script + registry
directly (needs `webfs` for TLS, the default):

```rc
hget https://raw.githubusercontent.com/Alino/agent9/main/pac9/pac9 >/rc/bin/pac9
chmod +x /rc/bin/pac9
mkdir -p /sys/lib/pac9
hget https://raw.githubusercontent.com/Alino/agent9/main/pac9/registry >/sys/lib/pac9/registry
```

Or, if you already have this directory (cloned the repo):

```rc
rc install.rc      # cp pac9 -> /rc/bin, cp registry -> /sys/lib/pac9
```

## Registry

`/sys/lib/pac9/registry`, tab-separated:
`name<TAB>url<TAB>subdir<TAB>recipe<TAB>deps`.

- `subdir` `.` = repo root (use e.g. `src/vts` for a package inside a monorepo).
- `recipe` `-` = default detection; otherwise an rc command run in the package
  dir (e.g. netsurf's `fetch clone http; mk; mk install`).
- `deps` optional 5th column: space-separated package names installed first if
  not already present. Omit it or use `-` for none.

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

`rc test.rc` appends throwaway packages to the registry and checks install,
dependency resolution, the manifest, and uninstall — no git or network needed.
Run it on the 9front box; it prints `PASS` or `FAIL: <reason>`.
