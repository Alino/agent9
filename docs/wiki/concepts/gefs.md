---
title: GEFS — Good Enough File System
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [plan9, fs, 9p]
sources: [raw/articles/9front-release-gefs-sp1-2026-01.md, raw/articles/9front-release-this-time-definitely-2025-01.md]
---

# GEFS — Good Enough File System

The new default filesystem in [[9front-release-status|9front]] since
2025-01 ("THIS TIME DEFINITELY"). Designed by sirjofri + cinap_lenrek + qwx.
Mounted as `gefs(4)`.

## Design goals

- **Crash safe without fsck.** Power loss should leave a consistent
  filesystem. No multi-hour scrub on boot.
- **Snapshots cheap and named.** Copy-on-write, similar in spirit to ZFS
  and bcachefs but much smaller.
- **Detect corruption.** Checksums on disk; not just trust-the-block-device.
- **Plan 9 idioms.** 9P file server, runs in userspace as a single process.

## Comparison to predecessors

| Feature | cwfs64x | fossil+venti | **gefs** |
|---|---|---|---|
| Default in installer | pre-2025 | (never) | **since 2025-01** |
| Crash recovery | replay on boot | venti-side | inherent (CoW) |
| Snapshots | hourly dump → venti | yes, via venti | **timed, in-tree** |
| Architecture | "worm" (write-only block log) | active+archive | **B-tree of snapshots** |
| External archive needed | yes (kfscmd dump) | yes (venti server) | **no** |

cwfs64x still works and ships. GEFS is the path forward.

## Architecture (the one-page version)

> "A GEFS file system consists of a snapshot tree, which points to a number
> of file system trees. The snapshot tree exists to track snapshots."
> — sys/doc/gefs.ms

Two trees:

1. **Snapshot tree** — maps snapshot name → root of a filesystem tree.
2. **Filesystem tree** — actual files and directories.

Both are B-trees, both are copy-on-write. A new snapshot is a new root
pointer in the snapshot tree pointing at the (immutable) current root of
the filesystem tree.

## Timed snapshots (added April 2025 release)

This was the missing piece in the original 2025-01 ship. The April mid-year
release wired GEFS into the dump scheduler, so you get the venti-style
"hourly/daily/permanent" snapshot rhythm without needing a separate venti
server.

## What this changes for us

- **No separate venti.** Old plan9-agent setup booted a venti companion VM
  for dumps. Not needed anymore — single VM is fine.
- **Re-image rolls forward without ceremony.** Old cwfs64x disk images need
  migration (export, fresh GEFS install, copy). Easier to start a new VM
  and clone source via [[git9]].
- **Less anxiety about VM crashes.** Crash during a `mk install`? Boot,
  filesystem is consistent, continue.

## Pitfalls

- GEFS is younger than cwfs64x — bug-fix velocity is still high. If you hit
  weird behaviour, check the changelog of the current release first.
- The "no fsck" promise depends on power-loss-resistant block storage. In
  QEMU with qcow2 + writeback cache, you can still corrupt things if the
  host crashes mid-write. Use `cache=none` or `cache=writethrough` if
  paranoid.

## Sources

- 9front sys/doc/gefs.ms (read in-tree on the VM, or at
  http://git.9front.org/plan9front/plan9front/HEAD/sys/doc/gefs.ms/f.html)
- DeepWiki: https://deepwiki.com/xplshn/9front
- The Register, 2025-04-29 coverage

## See also

- [[9front-release-status]]
- [[build-toolchain]]
