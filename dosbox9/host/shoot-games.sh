#!/bin/bash
# shoot-games.sh — run each game on the 9front target in turn and screenshot it.
#
#   ./shoot-games.sh [seconds-to-wait] [game ...]
#
# Output: dosbox9/screenshots/<game>.png + a one-line verdict per game.
#
# Capture is native (`topng < /dev/screen` on the box), not QEMU screendump, so
# this works on bare metal. rio serves /dev/screen per-WINDOW and a listen1
# session owns none, so shot.rc re-enters the game window by id — see shot.rc.
#
# Target defaults to cirno (bare metal). The QEMU VM works too but it's
# software-emulating x86 on ARM (no KVM/HVF), which is ~40x slower — too slow
# for a game to reach a title screen in a sane wait.
set -uo pipefail
D9="$(cd "$(dirname "$0")/.." && pwd)"
VM_HOST="${VM_HOST:-192.168.88.159}"
VM_PORT="${VM_PORT:-17010}"
SHOTS="$D9/screenshots"
WAIT="${1:-25}"; shift || true
GAMES="${*:-keen1 keen4 jill raptor blake duke2 bmenace caves harry hocus wolf3d doom}"

mkdir -p "$SHOTS"
s() { printf '%s\n' "$1" | nc -w "${2:-12}" "$VM_HOST" "$VM_PORT" 2>/dev/null | head -3; }

pass=0; fail=0
for g in $GAMES; do
  s 'rc /tmp/d9/cleanup.rc' 20 >/dev/null
  sleep 2
  s "rc /tmp/d9/rungame.rc $g &" 6 >/dev/null
  sleep "$WAIT"

  alive=$(printf 'ps | grep -c dosbox\n' | nc -w 8 "$VM_HOST" "$VM_PORT" 2>/dev/null | head -1 | tr -d ' \r')
  s "rc /tmp/d9/shot.rc /tmp/d9/shot-$g.png" 25 >/dev/null
  printf "cat /tmp/d9/shot-$g.png\n" | nc -w 25 "$VM_HOST" "$VM_PORT" > "$SHOTS/$g.png" 2>/dev/null
  sz=$(stat -f%z "$SHOTS/$g.png" 2>/dev/null || echo 0)

  if [ "${alive:-0}" -ge 1 ] 2>/dev/null && [ "$sz" -gt 2000 ]; then
    echo "PASS $(printf '%-8s' "$g") procs=$alive shot=${sz}B"
    pass=$((pass+1))
  else
    err=$(printf "grep -i 'exit to error' /tmp/d9/$g.log\n" | nc -w 8 "$VM_HOST" "$VM_PORT" 2>/dev/null | head -1)
    echo "FAIL $(printf '%-8s' "$g") procs=${alive:-0} shot=${sz}B ${err}"
    fail=$((fail+1))
  fi
done
s 'rc /tmp/d9/cleanup.rc' 20 >/dev/null
echo "--- $pass ok, $fail failed — screenshots in $SHOTS"
