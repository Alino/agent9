---
title: 9front Release Status
created: 2026-05-17
updated: 2026-05-17
type: reference
tags: [plan9, fs, toolchain]
sources: [raw/articles/9front-release-gefs-sp1-2026-01.md, raw/articles/9front-release-this-time-definitely-2025-01.md]
---

# 9front Release Status

What's current upstream, and what we should be running in our VM.

## Timeline

| Date | Release | Headline |
|---|---|---|
| 2025-01-19 | **THIS TIME DEFINITELY** | [[gefs]] enabled by default in installer; CVE-2024-8158 fixed; dhcpv6 in ipconfig |
| 2025-04 | (unnamed mid-year release) | Timed snapshots for GEFS — makes snapshots a real working feature |
| **2026-01-24** | **GEFS SERVICE PACK 1** (current) | Shell scripts as `#!` interpreter; namespace perf fixes; libdraw cleanup; lots of devip/devmnt polish |

Release ID for current: **11554**. ISO/qcow2 names embed it
(`9front-11554.amd64.iso.gz`).

## What's relevant for plan9-winxp

### Namespace performance (SP1)

> Eliminate `mountid` and quadratic algorithms in `pgrpcpy()` and
> `devproc/readns1()`.

[[mxio-design]] and [[pi9-architecture]] both fork heavily and build big
namespaces. The pre-SP1 kernel had O(n²) work in `pgrpcpy()` proportional
to namespace size. SP1 fixes that. Worth re-imaging the dev VM.

### libdraw cleanup (SP1)

- Display locking cleanup
- `icossin2()` integer overflow fix
- `menuhit()/emenuhit()` and `enter/eemter()` generalised
- Image area limit removed from `badrect(2)`

[[draw-api]] consumers (mxio decorations, xena-panel, vtwin) inherit these
for free. If we hit weird drawing artifacts on the old image, suspect the
old kernel first.

### Shell scripts as `#!` interpreter (SP1)

Previously `#!/bin/rc` worked in some contexts but not all. SP1 makes it
universal — relevant if pi9's `run_rc` tool ends up wrapping rc snippets.

### `utfncmp()` (SP1)

New libc function. Plain useful, no specific consumer yet.

### ether82563 + new wifi (SP1)

i219-lm/i219-v (ThinkPad L13 Gen 1) ethernet works now. Not relevant for
the QEMU VM, but relevant if Alex ever wants 9front on a real laptop.

## What we DON'T get from upgrading

The four components ([[mxio-design]], [[xena-panel-design]], launcher,
xfiles) live entirely in userspace. None of them depend on SP1 features —
they'll all compile and run on cwfs64x + 2024-era kernel too.

Recommendation: re-image only if you hit something the changelog explains.
Don't churn for its own sake.

## VM image source

```
hget http://9front.org/iso/9front-11554.amd64.qcow2.gz | gunzip > disk.qcow2
```

SHA256 (amd64 qcow2):
`0e4a0808020c7845f854599b910d3a63ee56cbf3ebcd038332e22b7c1a272361`

See [[build-toolchain]] for QEMU boot command.

## See also

- [[gefs]] — the filesystem that named both releases
- [[git9]] — Ori's git ships in this release
- [[build-toolchain]] — how we build/test against 9front
