---
source_url: https://github.com/oridb/git9
ingested: 2026-05-17
sha256: snapshot-only
---

# git9 — Plan 9 native git client

By Ori Bernstein. 208 stars. C (79.5%), Shell (13.7%), Roff (6.5%).
Latest commit May 27 2024: "git: sync with 9front". MIT.

## Why it exists

> "Plan 9 is a non-posix system. Upstream git has been ported, but feels
> distinctly un-plan9ish, and even in its native environment, has been
> justifiably tarred and feathered for its user experience."

git9 ships in 9front (`git/clone`, `git/log`, etc. are git9 commands).
The github repo is the upstream — 9front periodically syncs from it.

## Architecture

`git/fs` mounts a read-only 9P filesystem at `$repo/.git/fs`. Surrounding
scripts/binaries mutate the repo on disk; the filesystem mirrors those
changes immediately. No daemon, no IPC layer — the wire is 9P, like
everything else on Plan 9.

### Filesystem layout

```
$repo/.git/fs/object   the objects in the repo
$repo/.git/fs/branch   the branches in the repo
$repo/.git/fs/ctl      file showing repo status (current branch)
$repo/.git/fs/HEAD     alias for the currently checked-out commit directory
```

Commits are directories containing:
- `author`
- `hash`
- `parent` (one per line)
- `msg`
- `tree` (directory view of repository at that commit)

## Key differences from upstream git

- **No index / no staging area.** Considered "boneheaded, confusing and clunky."
- Three file states only: `untracked`, `dirty`, `committed`.
- Tracking via empty marker files: `.git/index9/{removed,tracked}/path/to/file`.
- Implemented in Plan 9 flavor C.
- **Wire-compatible with upstream git.** Caveat: if you share the same
  physical repo across Plan 9 and Unix, the two clients will disagree about
  modified files (different on-disk index format).

## Daily-driver usage

```
git/clone git://git.eigenstate.org/ori/mc.git
git/log
cd subdir/name
git/add foo.c
diff bar.c $repo/.git/fs/HEAD/
git/commit foo.c
git/push
```

Diff against a branch (until `git/diff` is improved):

```
ape/diff -ur $repo/.git/fs/branch/heads/master/tree .
```

Browse a branch as files:

```
ls $repo/.git/fs/branch/heads/master
cat $repo/.git/fs/branch/heads/master/hash
```

## Commands

| Tool | Purpose |
|---|---|
| `fs` | the git filesystem |
| `fetch` | wire protocol get |
| `send` | wire protocol push |
| `save` | store files for commit |
| `conf` | extract from config file |
| `clone` | clone a repo |
| `commit` | snapshot selected files |
| `log` | print commit log |
| `add` | stage for next commit |
| `walk` | `du`-but-for-status |

Protocols: `git://` and `git+ssh://`. Patches welcome for others.

## Install (inside 9front)

Bootstrap:
```
cd /tmp
hget https://orib.dev/git/git9/HEAD/snap.tar.gz | tar xvz
cd git9
mk all
mk install
```

Configure identity:
```
mkdir -p $home/lib/git
echo '
[user]
    name=Alexander Sadovsky
    email=glenda@example.com
' > $home/lib/git/config
```

Docs: `man 1 git`, `man 4 gitfs`.

## 9legacy notes

git9 targets 9front. On 9legacy you need patches for: rc-line-split
(`delim{...}` syntax), walk command port, aux/getflags 9front version with
named args. git9 also defaults to `hold(1)` as the editor — not on 9legacy,
either import it or set `$editor`.

## Contributors

oridb, okvik, michaelforney, mischief, fhs, ddevault, iru-.
