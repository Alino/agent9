---
title: git9 — Plan 9 native git
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [plan9, 9p, toolchain]
sources: [raw/articles/git9-readme-2026-05.md, raw/articles/oridb-repos-overview-2026-05.md]
---

# git9 — Plan 9 native git

The git client that ships in [[9front-release-status|9front]]. By
Ori Bernstein ([[oridb-ecosystem]]). MIT. 208 stars upstream.

## What it is

A **9P file server** that exposes a git repository as a mountable
filesystem at `$repo/.git/fs`, plus a set of small commands that read
and mutate it. Wire-compatible with upstream git — clone/push/pull across
the same `git://` and `git+ssh://` protocols.

It's not a port of upstream git. It's a from-scratch rewrite in Plan 9
flavor C, designed around 9P. The author's stated motivation:

> "Plan 9 is a non-posix system. Upstream git has been ported, but feels
> distinctly un-plan9ish, and even in its native environment, has been
> justifiably tarred and feathered for its user experience."

## Filesystem layout

```
$repo/.git/fs/object   the objects in the repo
$repo/.git/fs/branch   the branches in the repo
$repo/.git/fs/ctl      current branch + repo status
$repo/.git/fs/HEAD     alias for the currently checked-out commit
```

Commits are directories with `author`, `hash`, `parent`, `msg`, `tree`.
You can `ls $repo/.git/fs/branch/heads/master/tree` and walk the
committed state as plain files.

## The big philosophical difference

**No index. No staging area.** Three file states only: `untracked`,
`dirty`, `committed`. Marker files in `.git/index9/tracked/...` and
`.git/index9/removed/...` replace the binary index.

`git/add` adds a file to the next commit's manifest. `git/commit foo.c`
commits exactly `foo.c` (no `git add` dance first). Wins simplicity,
loses partial-hunk commits.

## Daily-driver flow

```rc
git/clone git://git.eigenstate.org/ori/mc.git
git/log
git/add foo.c
git/commit foo.c
git/push
```

Diff against current HEAD (until `git/diff` improves):

```rc
ape/diff -ur $repo/.git/fs/HEAD/tree .
```

## Commands

| Command | Purpose |
|---|---|
| `git/fs` | the 9P filesystem |
| `git/fetch` | wire protocol fetch |
| `git/send` | wire protocol push |
| `git/save` | store files for commit |
| `git/conf` | extract from config file |
| `git/clone` | clone a repo |
| `git/commit` | snapshot selected files |
| `git/log` | print commit log |
| `git/add` | mark for next commit |
| `git/walk` | `du`-style status |

## Relevance to plan9-winxp

### Inside the VM

git9 is how we'll pull source onto the 9front VM. Either:

1. Bootstrap snapshot:
   ```
   cd /tmp
   hget https://orib.dev/git/git9/HEAD/snap.tar.gz | tar xvz
   cd git9 && mk all && mk install
   ```
2. Updatable git clone (once git9 is installed):
   ```
   cd $home/src
   git/clone git://github.com/<user>/plan9-winxp
   ```

Configure identity in `$home/lib/git/config`.

### As a design reference

git9 is one of the cleanest examples of "expose state as a 9P filesystem
instead of writing a CLI" in current 9front. [[xena-panel-design]] uses
the same pattern (panel state as `/mnt/taskbar/`) and so will pi9's
service-composition tools ([[plan9-namespaces-for-agents]]). Reading
git9's `fs.c` is worth an afternoon — it's a small, idiomatic 9P server.

### Sync-with-9front model

Latest commit message (2024-05-27) is literally "git: sync with 9front".
git9 lives on github but **9front periodically syncs from it into the
main tree**. Same pattern Ori uses for his plan9port fork — github as
upstream, 9front mirror downstream.

## Cross-platform sharing pitfall

If you share the same physical repo across Plan 9 and Unix (e.g. via a
shared mount), the two clients **will disagree about modified files** —
different on-disk index format. Wire protocol is fine; on-disk metadata
is not. Either clone twice or don't cross-mount.

## Sources

- https://github.com/oridb/git9 — the upstream
- https://orib.dev/git/git9/HEAD/ — snapshot tarballs
- `man 1 git` / `man 4 gitfs` inside 9front

## See also

- [[oridb-ecosystem]]
- [[9front-release-status]]
- [[xena-panel-design]] — same "state-as-9P-fs" pattern
- [[plan9-namespaces-for-agents]] — pi9 uses this idiom for tools
