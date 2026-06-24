#!/bin/bash
# run-macos.sh — boot agent9 on macOS.
#
# Requires: qemu (brew install qemu)
# Optional: SDL display for snappier rendering than the default Cocoa
#
# At first boot you'll see two prompts:
#   1. bootargs is (...) [default]  -> press Enter
#   2. user[glenda]:                -> press Enter
# Then mxio + xena-panel + a vtwin terminal come up. Click "Start" in
# the taskbar to launch Pi9, NetSurf, or another terminal.
set -e
cd "$(dirname "$0")"

if [ ! -f agent9-v0.3.0.qcow2 ]; then
  echo "error: agent9-v0.3.0.qcow2 not found in $(pwd)"
  echo "download it from https://github.com/Alino/agent9/releases"
  exit 1
fi

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
  echo "error: qemu not installed."
  echo "install with: brew install qemu"
  exit 1
fi

# Apple Silicon: qemu-system-x86_64 runs in TCG (no HVF cross-arch).
# Intel Mac: --accel hvf works and roughly triples speed; left off here
# because the slow-down on Apple Silicon is the bottleneck most users
# hit, and the script should "just work" on both.

# Hostfwd ports:
#   2222 ssh   (glenda has no password by default; nothing listens)
#   1717 listen1 (rc shell, dev convenience)
#   1564 9P    (mount this VM's namespace from another host)
#  53692 OAuth callback (pi9 /login: Anthropic / GitHub Copilot)
#   1455 OAuth callback (pi9 /login: OpenAI ChatGPT/Codex)

exec qemu-system-x86_64 \
  -name agent9 \
  -m 2048 \
  -smp 2 \
  -cpu max \
  -drive file=agent9-v0.3.0.qcow2,if=virtio,format=qcow2 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::1717-:17010,hostfwd=tcp::1564-:564,hostfwd=tcp::53692-:53692,hostfwd=tcp::1455-:1455 \
  -device virtio-net-pci,netdev=net0 \
  -device virtio-rng-pci \
  -usb \
  -device usb-tablet \
  -display "${DISPLAY_BACKEND:-cocoa}"
