---
source_url: http://9front.org/releases/2026/01/24/0/
ingested: 2026-05-17
sha256: snapshot-only
---

# 9FRONT "GEFS SERVICE PACK 1" — 2026-01-24

Release ID: 11554. Installation media for PC (386/amd64 ISO + qcow2),
Raspberry Pi (1/2/3 + 3/4), MNT Reform, Honeycomb, QEMU (amd64+arm64 qcow2).

## Notable user-visible changes

- `exec()` now supports shell-scripts as `#!` interpreter (works for rc and
  external shells called from inside scripts).
- `image/histogram` shipped.
- Sam scrolling improved.
- `stats` auto-reconnects on link drop.
- `memaffinewarp` now supported on windows (memlayer).
- New `/net/*/trans` per-protocol read/writable transport file (devip).
- `utfncmp()` added to libc.
- `ircrc`: TLS by default.

## Kernel highlights

- `Lock` definition moved into portable code.
- Allow scripts as `#!` interpreter in `sysexec()`.
- Debounce `killbig()` (memory-pressure killer no longer storms).
- Fix `namec()` Aunmount semantics for directories.
- Fix off-by-one in sysexec progargs.
- Fix `waserror()` handling for `bindmount()` and `sysunmount()`.
- Eliminate `mountid` and quadratic algorithms in `pgrpcpy()` and
  `devproc/readns1()` — namespace-heavy programs (rio sessions, pi9-style
  agents) get a real-world speedup.
- Refactor `chdev(1)` machinery + `devallowed()` check in `namec()` for `#` devices.

## Devices

- **devdraw**: use `memlaffinewarp(2)` for affinewarp rpc.
- **devether**: eliminate loopback; talk to devices sharing own MAC address.
- **devip**: add read/writable per-protocol `trans` file; handle all loopback
  traffic with `loopbackmedium`; handle announce/connect ctl error;
  `Fs ipfs[]` and `Queue qlog` now static.
- **devmnt**: don't spam console with mountio errors; implement bread/bwrite
  handlers.

## Ethernet/Wireless

- **ether82563**: add device ids `0d4e` (i219-lm) + `0d4f` (i219-v) →
  ThinkPad L13 Gen 1 ethernet now works.
- **etheriwl**: disable power saving in promisc mode; Centrino Advanced-N
  6200 0x422c support.

## USB

- nusb/disk: ignore file offset for raw status reads; mode switching for
  Realtek RTL8153 NICs.
- nusb/ether/asix.c: AX88179A support; phy status register for link checking.
- nusb/joy: shanwan controllers in xbox360 mode.

## Platform

- pc64: `vmap()` works beyond 1TB (noam).
- xen: remove `fpoff()` call in `execregs()`.
- zynq: postnote "sys: breakpoint" on debug exception; `#P/temp` → `#P/cputemp`.

## TCP

- Splice local tcp conversations together, bypassing ip stack.
- Avoid backoff counter overflow, reorder fields in `Tcpctl`.

## Compilers / libc

- `?c`: eliminate `.rathole` for arm/arm64/386/arm-thumb/mips; grow regions
  dynamically.
- `cc`: hint for register pair; init bitfields; address of incomplete type.
- libc: add `utfncmp()`, smoketests for locking, mark `exits` as
  profile-able, libc/arm memmove early-return when pointers equal.

## libdraw

- Display locking cleanup.
- Fix `icossin2()` integer overflow.
- Generalise `menuhit()/emenuhit()` and `enter/eemter()` functions.
- Remove image area limit from `badrect(2)`.

## SHA256s (amd64)

```
5aaf54327b4bb73a17e192488dc3e65d9d8e526728732e2fdf402bccb8c60236  9front-11554.amd64.iso.gz
0e4a0808020c7845f854599b910d3a63ee56cbf3ebcd038332e22b7c1a272361  9front-11554.amd64.qcow2.gz
```

## Resources

- Dash 1 manual (PDF): http://fqa.9front.org/dash1.gefs-sp1.pdf
- Git: http://git.9front.org/plan9front/9front/HEAD/info.html
- Release song "enpassant" by qwx: http://nopenopenope.net/mus/12/enpassant.r04.flac
