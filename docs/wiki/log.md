---
title: Local installation log
created: 2026-10-10
updated: 2026-10-10
type: reference
tags: [toolchain, rc, 9p]
---

# Local installation log

2026-10-10: Installing agent9 on glenda's existing amd64 9front. Installed
libvterm and its missing headers and added opaque-type pragmas needed
by the current kencc linker. Documented this in concepts/build-toolchain.md.
These are local installation fixes; no upstream commit was created.

Added a checked native adapter for vterm_screen_get_cell because kencc
drops the incomplete-type marker when applying const. The original
const-qualified API and all ownership and synchronization remain intact.

2026-10-10: Prepared the tested compatibility changes in a separate local
checkout. The optional new-pi9 hook invokes the user's agent9-vts helper,
which serializes first startup; no personal credentials or home paths are
embedded in the project scripts. Existing startup behavior is unchanged
when that helper is absent. See [[build-toolchain]] and [[vt-architecture]].
