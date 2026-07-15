# pac9 — a package manager for 9front

One line to install software on a running 9front box:

```
pac9 install netsurf              # curated short name
pac9 install vts vtwin            # several at once
pac9 install pi9                  # pulls in its deps (vts, vtwin) too
pac9 install https://host/repo    # any git repo
pac9 list                         # name, version, source
pac9 uninstall netsurf
pac9 update                       # refresh the registry (and pac9 itself)
pac9 outdated                     # registry version != installed version
pac9 upgrade [name...]            # reinstall what moved (all, if unnamed)
pac9 changelog gl9                # what changed in the version you have
```

The registry is a local file, so a box only sees packages published before it
installed pac9 — run `pac9 update` when a name gives "bad uri" or you know a
new package shipped. `update` refreshes the *registry*; `upgrade` refreshes your
*packages*.

## Versions

A registry entry may carry a 6th column: its current version. When it does, the
release URL embeds that version, so **the URL is immutable** — a tag you can
overwrite is not a version: the same URL would quietly serve different bytes, and
nobody could pin a build or roll one back. pac9 records the version it installed,
which is what lets `list`, `outdated` and `upgrade` mean anything.

Entries without a version still work; they read as `-` and upgrade only via an
explicit `pac9 install`. Nothing that has no version on both sides is ever
reported outdated — with nothing to compare, saying so would be a guess.

Versioned packages ship a `CHANGELOG` (at `/sys/lib/pac9/changelog/<name>`), so
`pac9 changelog <name>` works offline and answers "why would I upgrade?".

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
   that target), else `mk`, else `build.rc`, else a POSIX build under APE
   (`ape/sh configure` if present, then `ape/make` + `ape/make install`), else it
   stops and tells you where the source is. A failed build is *not* recorded as
   installed.
5. **Record** — appends `name url srcdir` to `/sys/lib/pac9/installed`.

For an autotools/POSIX port with a non-default prefix or configure flags, put the
full command in a registry `recipe` (e.g. `ape/sh configure --prefix=/usr/local;
ape/make; ape/make install`) rather than relying on the default detection.

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
- **Prebuilt** (`url` is `-`) — no repo. Recipe `tarball <url>` makes pac9 fetch
  a release tarball, record every path it contains (in `/sys/lib/pac9/files/`),
  and unpack it at `/`. Used for the big cross-ports whose source isn't
  self-contained. The recorded file list is what makes them cleanly removable.

## python9 / node9 / zig9

These are large cross-ports (CPython, QuickJS+npm, Zig, clang/LLVM). Their
vendored upstream source is **not** in the repo and building it on a TCG VM takes
20–60 min, so they ship as **prebuilt tarball packages**: `pac9 install python9`
fetches the built binary + libdir and unpacks it at `/`. Point the placeholder
tarball URLs in `registry` at the real GitHub Release assets once published.

Because a `tarball` install records the file list it unpacked, `pac9 uninstall
python9` removes exactly those files (the tarball installs `python`, not
`python9`, so a name-based `rm` wouldn't find it). Empty package directories are
cleaned up; shared dirs like `/amd64/bin` are left alone.

## rust9 (the Rust toolchain)

`pac9 install rust9` (~80 MB) installs the real `rustc` (1.98-dev, cranelift
backend) **running on 9front**: compiler + plan9 std sysroot + the `n9link`
ELF→a.out linker + `cargo9`, laid out under `/usr/glenda/rust` with `rustc` /
`cargo9` wrappers in `/rc/bin`. Self-contained — it bundles its cc9 runtime
substrate, so it needs no other package (not even `cc9`). `rustc hi.rs -o hi`
then `./hi`, entirely on the box. Build the tarball with
`rust9/release/make-tarball.sh`; publish it as the `rust9` GitHub release.

## gl9 (OpenGL)

`pac9 install gl9` gives you OpenGL 3.3 (Mesa softpipe) on 9front. It's split in
two: **`gl9win`** — the native libdraw window server — is an ordinary *source*
package (cloned + `mk install`, builds on-box); **`gl9`** is a *tarball* of the
cross-compiled GL demos (`gl9-cube`, `gl9-egl`) plus the `gl9` launcher, and it
depends on `gl9win` (pulled in automatically). Then, from a rio window:

```
gl9 cube      # a spinning lit 3D cube
gl9 egl       # a triangle through the EGL API
```

Your own GL apps are cross-compiled on a host with cc9 (static build — there's no
shared libGL on 9front) and run with `gl9 run <app>`. Build the tarball with
`gl9/release/make-tarball.sh` and publish it as the `gl9` GitHub release.

## alacritty9 (the Alacritty terminal)

`pac9 install alacritty9` (~5 MB) installs real upstream Alacritty 0.17.0: the
cross-compiled binary (Mesa softpipe + OSMesa EGL + bundled Go Mono fonts all
statically linked, ~19 MB installed) under `/usr/glenda/alacritty9/`, the
`gl9win2` interactive window host in `/amd64/bin`, and the `alacritty9` launcher
in `/rc/bin`. Self-contained — no other package needed. From a rio window:

```
alacritty9              # rc in a real GPU-terminal, colors and all
alacritty9 -e pi9       # straight into pi9 (full-screen TUIs work)
```

Config lives at `$home/lib/alacritty/alacritty.toml`. Build the tarball with
`alacritty9/release/make-tarball.sh`; docs in `alacritty9/README.md`,
`PORT-NOTES.md` (patches, perf, limits) and `PROTOCOL.md` (the gl9win2 wire
format).

## Verify

`rc test.rc` appends throwaway packages to the registry and checks install,
dependency resolution, the manifest, and uninstall — no git or network needed.
Run it on the 9front box; it prints `PASS` or `FAIL: <reason>`.
