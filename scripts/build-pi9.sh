#!/usr/bin/env bash
# build-pi9.sh — cross-compile pi9 from macOS for plan9/amd64.
#
# Output: src/pi9/pi9.plan9.amd64 — copy into the 9front VM via hget
# from a Mac-side http.server (see scripts/serve-pi9.sh).
#
# Usage:
#   scripts/build-pi9.sh                  # cross-build only
#   scripts/build-pi9.sh --serve          # cross-build + start http.server
#   scripts/build-pi9.sh --host           # build host binary too (smoke test)

set -euo pipefail
cd "$(dirname "$0")/.."

cd src/pi9

go mod tidy >/dev/null

echo "==> cross-compile plan9/amd64"
GOOS=plan9 GOARCH=amd64 go build -o pi9.plan9.amd64 .

ls -la pi9.plan9.amd64

if [[ "${1:-}" == "--host" ]]; then
  echo "==> host build (smoke test only — Bubble Tea needs a real TTY to run)"
  go build -o pi9-host .
  ls -la pi9-host
  shift || true
fi

if [[ "${1:-}" == "--serve" ]]; then
  echo "==> serving pi9.plan9.amd64 on http://0.0.0.0:8765"
  echo "    inside the VM:"
  echo "        hget http://10.0.2.2:8765/pi9.plan9.amd64 > /tmp/pi9"
  echo "        chmod +x /tmp/pi9"
  echo "        /tmp/pi9"
  exec python3 -m http.server 8765 --bind 0.0.0.0
fi
