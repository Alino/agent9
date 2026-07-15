#!/bin/sh
# make-tarball.sh — assemble tailscale9-amd64.tar.gz for pac9's `tarball`
# install: upstream Tailscale cross-compiled for plan9/amd64 (Go emits native
# Plan 9 a.outs — no elf2aout step), laid out at /.
#
# VERSION below must match the registry's 6th column and the release tag must
# embed it (tailscale9-v$VERSION) so the download URL is immutable.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
VERSION=1.100.0
STAGE="$HERE/stage"; TARBALL="$HERE/tailscale9-amd64.tar.gz"

BIN="$(go env GOPATH)/bin/plan9_amd64"
GOOS=plan9 GOARCH=amd64 go install \
	-ldflags "-X tailscale.com/version.longStamp=$VERSION-agent9 -X tailscale.com/version.shortStamp=$VERSION" \
	tailscale.com/cmd/tailscaled@v$VERSION tailscale.com/cmd/tailscale@v$VERSION

rm -rf "$STAGE"
mkdir -p "$STAGE/amd64/bin" "$STAGE/rc/bin" "$STAGE/sys/lib/tailscale" \
         "$STAGE/sys/lib/pac9/changelog"
cp "$BIN/tailscale"  "$STAGE/amd64/bin/tailscale"
cp "$BIN/tailscaled" "$STAGE/sys/lib/tailscale/tailscaled"
cp "$HERE/tailscaled" "$STAGE/rc/bin/tailscaled"
chmod +x "$STAGE/amd64/bin/tailscale" "$STAGE/sys/lib/tailscale/tailscaled" \
         "$STAGE/rc/bin/tailscaled"
cp "$HERE/CHANGELOG" "$STAGE/sys/lib/pac9/changelog/tailscale9"

# ustar format so 9front's tar reads it (no pax/GNU extensions).
( cd "$STAGE" && tar --format ustar -czf "$TARBALL" amd64 rc sys )
echo "-> $TARBALL  ($(du -h "$TARBALL" | cut -f1))"
echo
echo "publish (tag MUST embed the version — the URL has to be immutable):"
echo "  gh release create tailscale9-v$VERSION '$TARBALL' \\"
echo "     -t 'tailscale9 $VERSION — Tailscale VPN on 9front' -F '$HERE/CHANGELOG'"
