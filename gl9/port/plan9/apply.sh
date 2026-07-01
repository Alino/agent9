#!/bin/sh
# apply.sh — apply gl9's Mesa patches to vendor/mesa, in order. fetch.sh re-extracts
# pristine Mesa, so run this after fetching (idempotent: skips already-applied).
# Patches are minimal — almost all porting is done via the harvest scrub +
# force-included shim (shim/gl9_pre.h), NOT by editing Mesa. See NOTES.md.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)          # gl9/port/plan9
MESA="$HERE/../../vendor/mesa"
[ -d "$MESA" ] || { echo "no vendor/mesa — run gl9/fetch.sh first"; exit 1; }

for p in "$HERE"/patches/*.patch; do
	[ -e "$p" ] || continue
	if patch -d "$MESA" -p1 --dry-run -R -f <"$p" >/dev/null 2>&1; then
		echo "already applied: $(basename "$p")"
	else
		echo "applying: $(basename "$p")"
		patch -d "$MESA" -p1 <"$p"
	fi
done
echo "patches applied."
