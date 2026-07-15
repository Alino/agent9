#!/bin/bash
# deploy.sh — ship dosbox.aout (+ the rc helpers) to a 9front box and, if a
# game is named, run it.
#
#   ./deploy.sh                 # just ship the binary to cirno
#   ./deploy.sh doom            # ship and run doom
#   VM_HOST=127.0.0.1 VM_PORT=1717 ./deploy.sh   # the QEMU VM instead
#
# Transfer is http+hget, NOT cc9/host/deliver.py: deliver.py embeds the binary
# as a C byte array and compiles it on the box — ~24MB of C for a 3.5MB a.out.
# Serve $SHIP yourself first:  (cd $SHIP && python3 -m http.server 8801)
#
# Default target is cirno (bare metal). The QEMU VM works but has no KVM/HVF
# here, so it software-emulates x86 on ARM (~40x slower) — too slow for games.
set -uo pipefail
D9="$(cd "$(dirname "$0")/.." && pwd)"
VM_HOST="${VM_HOST:-192.168.88.159}"
VM_PORT="${VM_PORT:-17010}"
SHIP="${SHIP:-/tmp/dosbox9-ship}"
HTTP="${HTTP:-192.168.88.10:8801}"   # how the GUEST reaches this host's server
GAME="${1:-}"

mkdir -p "$SHIP"
s() { printf '%s\n' "$1" | nc -w "${2:-15}" "$VM_HOST" "$VM_PORT" 2>/dev/null | head -3; }

cp "$D9/_out/dosbox.aout" "$SHIP/dosbox.aout"
cp "$D9/port/plan9/"*.rc "$SHIP/"

s 'rc /tmp/d9/cleanup.rc' 20 >/dev/null
s 'mkdir /tmp/d9' 6 >/dev/null
s "hget http://$HTTP/dosbox.aout > /tmp/d9/dosbox" 90 >/dev/null
s 'chmod +x /tmp/d9/dosbox' 6 >/dev/null
for f in rungame.rc shot.rc cleanup.rc; do
  s "hget http://$HTTP/$f > /tmp/d9/$f" 20 >/dev/null
done
s 'ls -l /tmp/d9/dosbox' 6

[ -n "$GAME" ] || exit 0
s "rc /tmp/d9/rungame.rc $GAME &" 6 >/dev/null
echo "launched $GAME — screenshot with: rc /tmp/d9/shot.rc /tmp/d9/shot.png"
