---
source_url: http://9front.org/releases/2025/01/19/0/
ingested: 2026-05-17
sha256: snapshot-only
---

# 9FRONT "THIS TIME DEFINITELY" — 2025-01-19

Prior release before GEFS SP1. The release that flipped the default.

## Headline changes

- **GEFS (4) enabled in the installer by default.** Previous installer
  defaulted to cwfs64x (the old venti-backed filesystem). GEFS is now the
  recommended choice for fresh installs.
- **CVE-2024-8158 fixed.**
- `ip/ipconfig` now supports dhcpv6 dynamic allocations and prefix
  expiration handling.

## What this means for us

When we re-imaged the 9front VM, we ended up on GEFS without thinking about
it — that's the cause. The April 2025 release (between "THIS TIME DEFINITELY"
and "GEFS SERVICE PACK 1") shipped timed snapshots for GEFS, making it the
real working filesystem rather than a tech preview.

## Article-form coverage

- The Register, 2025-04-29: "OpenBSD 7.7 is out, and so is the second
  9Front of 2025" — coverage of the timed-snapshots release. Compares GEFS
  to ZFS / bcachefs for copy-on-write snapshot support and crash-safe-without-fsck
  design.
- OSnews, 2025-01: announcement coverage of "THIS TIME DEFINITELY".
