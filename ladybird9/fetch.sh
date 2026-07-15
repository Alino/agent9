#!/bin/sh
# Reproducibly fetch the exact Ladybird we port. Pinned to a master commit —
# upstream has NO releases (pre-alpha) and is maintainers-only since June 2026,
# so this is a permanent pinned fork; the SHA below IS the version.
#
# WHY this pin (2026-07-15 master HEAD): the port targets current master by
# decision (docs/plans/2026-07-15-ladybird9-browser.md): it carries the AsmInt
# generated-assembly LibJS interpreter (the portable C++ interpreter was removed
# upstream in June 2026, PR #10099) and the Compositor process. Both are in
# scope; parity with THIS commit is the contract — patches in port/patches/
# apply on top and must stay surgical/additive.
#
#   vendor/ladybird/   the pinned tree (blobless clone; history kept so the
#                      emergency interpreter-revert fallback stays possible)
set -e

LADYBIRD_REV=8cc5d7a5ff3b2118a4f94e9aaafb1868c56eea11   # master @ 2026-07-15
LADYBIRD_URL=https://github.com/LadybirdBrowser/ladybird.git

HERE=$(cd "$(dirname "$0")" && pwd)            # ladybird9/
VENDOR="$HERE/vendor"
mkdir -p "$VENDOR"

if [ -d "$VENDOR/ladybird/.git" ]; then
	echo "already present: $VENDOR/ladybird"
else
	# blobless clone: full history metadata (needed to inspect/revert upstream
	# commits, e.g. PR #10099), blobs fetched lazily -> fast + small.
	git clone --filter=blob:none "$LADYBIRD_URL" "$VENDOR/ladybird"
fi

cd "$VENDOR/ladybird"
git fetch origin "$LADYBIRD_REV" 2>/dev/null || true
git checkout --detach "$LADYBIRD_REV"
echo "ladybird pinned at $(git rev-parse HEAD)"

# Apply the plan9 patch series (idempotent: reset to pin first).
if ls "$HERE"/port/patches/*.patch >/dev/null 2>&1; then
	git checkout -- . && git clean -fd >/dev/null
	for p in "$HERE"/port/patches/*.patch; do
		echo "applying $(basename "$p")"
		git apply --index "$p" || { echo "PATCH FAILED: $p" >&2; exit 1; }
	done
fi

# Binary assets can't live in a source patch. Bundled fonts: Plan 9 has no
# fontconfig and SerenitySans has no monospace, so ResourceFiles.cmake (patch
# 0004) references DejaVuSansMono.ttf — drop it in after patching.
if [ -d "$HERE/port/assets/fonts" ]; then
	cp "$HERE"/port/assets/fonts/*.ttf "$VENDOR/ladybird/Base/res/fonts/"
	echo "copied bundled fonts: $(ls "$HERE"/port/assets/fonts/)"
fi
