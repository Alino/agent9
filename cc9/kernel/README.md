# Optional W^X / JIT for 9front amd64 — `wxallow` + `SG_EXEC`

A minimal, **opt-in** kernel patch that lets a process request executable
writable memory (the prerequisite for a JIT — V8, LuaJIT, etc.) on otherwise
stock 9front amd64. **Secure by default**: with the gate off, the kernel behaves
exactly like stock (NX enforced on all writable memory). Nothing becomes
executable unless *both* a global gate is enabled *and* the process explicitly
asks per-segment.

This reverses the earlier "no userland JIT on stock 9front" conclusion: the
mechanism was already present (`SG_NOEXEC` + the `PTENOEXEC` bit); we only added
a gated way to *not* set it.

## The two controls

1. **Runtime gate — `wxallow`** (plan9.ini): a kernel global, default 0. Set
   `wxallow=1` in plan9.ini to allow executable segments. Absent/0 = stock NX
   everywhere.
2. **Per-segment opt-in — `SG_EXEC` (04000)**: a new `segattach` attr bit. A
   segment is executable only if it was created with `SG_EXEC`. Everything that
   doesn't ask (every normal program, the stack, bss, heap) stays NX — the
   macOS `MAP_JIT` model.

So a page is executable iff `wxallow==1` **AND** the segment was `segattach`'d
with `SG_EXEC`. A compromised process can still opt in, so "gate on" is a
genuinely lower-security mode — that is inherent to allowing JIT anywhere; the
per-segment flag just limits the blast radius to code that asks.

## Verified (QEMU dev VM, 2026-06-25), full truth table

| `wxallow` | segment            | result               |
|-----------|--------------------|----------------------|
| 1         | `segattach(SG_EXEC)` | **code runs (42)**   |
| 1         | plain `segattach`    | faults (NX)          |
| 0         | `segattach(SG_EXEC)` | faults (SG_EXEC stripped → NX) |

Probe: `exec_probe_jit.c` (writes `b8 2a 00 00 00 c3` = `MOVL $42,AX; RET` into a
segment and calls it).

## The patch (4 files, ~12 lines) — see `wxallow-jit.diff`

- **port/portdat.h** — add `SG_EXEC = 04000` to the segment-flags enum.
- **port/segment.c** — define `int wxallow;`; in `segattach()`, strip `SG_EXEC`
  from the attr unless `wxallow` is set (the gatekeeper).
- **port/fault.c** — in the two `PTENOEXEC` decisions, don't set the NX bit when
  the segment has `SG_EXEC`:
  `... && (s->type & SG_EXEC) == 0`.
- **pc64/main.c** — `extern int wxallow;`; in `confinit()` read it from plan9.ini
  via `getconf("wxallow")`.

## How to apply + install (the gotchas)

Build on the box (kencc), **not** cross-compiled:

```rc
cd /sys/src/9/pc64
mk
```

**CRITICAL: 9boot loads the kernel from the 9fat partition, NOT /amd64/9pc64.**
Installing only to `/amd64/9pc64` (what `mk install` / intuition suggest) boots
the *old* kernel. Copy to 9fat:

```rc
9fs 9fat
cp /sys/src/9/pc64/9pc64 /n/9fat/9pc64    # this is what actually boots
cp /sys/src/9/pc64/9pc64 /amd64/9pc64      # keep in sync
```

Enable the gate (optional — leave out for secure default):

```rc
9fs 9fat
echo wxallow=1 >> /n/9fat/plan9.ini
```

Reboot (`fshalt -r`). This dev VM's boot is interactive (bootargs prompt, then
`user[glenda]`); a headless reboot needs those answered (e.g. QEMU
`sendkey ret`) — or configure auto-login for true unattended boot.

## Userspace use

```c
/* request RWX memory; only honored if the kernel's wxallow gate is on */
void *p = segattach(0x800 /*SG_EXEC*/, "memory", 0, 0x1000);
/* ... write machine code into p, cast to a function pointer, call it ... */
```

Upstream 9front is unlikely to accept a W^X relaxation even gated, so carry this
as a local patch on the JIT-enabled image. cc9 static C++ binaries are
unaffected either way (they never request `SG_EXEC`), so the binary ecosystem
stays unified — only a JIT (e.g. V8) needs the patched kernel + `wxallow=1`.
