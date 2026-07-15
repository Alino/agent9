# ladybird9 — Ladybird browser on 9front

Port of [Ladybird](https://github.com/LadybirdBrowser/ladybird) (the from-scratch
C++23 browser) to Plan 9 / 9front, via the cc9 toolchain. Parallel browser track
to servo9; they share cc9/ssl9/gl9 substrate.

**Pin:** upstream master `8cc5d7a5ff` (2026-07-15). Upstream has no releases and
is maintainers-only → this is a permanent pinned fork; `fetch.sh` reproduces the
tree and applies `port/patches/`.

**Parity contract:** 1:1 with the pin. Additive platform backends only
(`AK_OS_PLAN9` arms, `TransportPlan9`, `UI/Plan9`), mirroring how the
Windows/Android arms landed. Deferred features (video, WebGL) go through
upstream's own gates and are tracked in `parity/`. Engine milestones are
measured against the same-commit host build (test262 / LibWeb tests / WPT
smoke), not asserted.

Full plan: `docs/plans/2026-07-15-ladybird9-browser.md`.

## Status

M0 — all three structural walls de-risked with on-box gates (dev VM + bare-metal
cirno), 2026-07-15:

| Wall | Proof |
|---|---|
| Cross-process shared memory | `cc9/runtime/shm9.c` over 9front `#g` named segments; `segshm_gate` **10/10** on VM + cirno. Kernel cap 60–64 segments system-wide → Phase B pool allocator planned (wire format already carries `offset`). |
| fd passing / IPC transport | `/srv` = genuine SCM_RIGHTS semantics: `srvfd_gate` **5/5**; full TransportPlan9 wire protocol (Segment/Srv attachments, 8 MB full-duplex deadlock probe, 10k soak) in `transport_echo_gate` **5/5**. Backed by new cc9 write-rings (honest POLLOUT) + real `posix_spawn` (`spawn_gate` **5/5**), cc9 regression suite 11/11. |
| AsmInt (generated-asm LibJS interpreter) | Assembles clean through cc9-c++ (184 handlers), audit: no TLS, no red zone, no syscalls; links + loads on cirno (`test/m0/asmint-spike.sh`). Offsets static_assert gate lands with M1's header work. |

Host reference build of the pin (parity baseline): `Ladybird.app` + `js` via
`Meta/ladybird.py build js` (needs brew nasm, autoconf, automake, libtool,
autoconf-archive; there is NO `headless-browser` target on master).

## Layout

    fetch.sh          pin + clone (vendor/, gitignored) + apply port/patches/
    host/             build scripts; deliver9.py (unique on-box scratch names —
                      plain deliver.py races other agent sessions on /tmp/cc9bin)
    port/patches/     the permanent fork surface, one purpose per patch
    test/m0/          wall-gate spikes (cc9-side gates live in cc9/test/)
    parity/           measured-parity ledgers vs the same-commit host build
    _out/, vendor/    gitignored build products + upstream tree

## Next

M1: `js` REPL on 9front — AK_OS_PLAN9 platform arm, AK/StackInfo + LibGC
BlockAllocator plan9 backends, ICU (archive data packaging; LibJS links
LibUnicode PUBLIC so Intl is not stubbable), the 10-crate Rust workspace via
rust9, asm-offsets static_assert gate, test262 smoke diffed against host.
