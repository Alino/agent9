#!/bin/bash
# run-linux.sh — boot agent9 on Linux.
#
# Requires: qemu-system-x86 (apt install qemu-system-x86, dnf install
# qemu-system-x86, etc.)
#
# Uses KVM acceleration when available (3-5x faster than TCG). Falls
# back to TCG if /dev/kvm is missing or your user isn't in the kvm
# group.
set -e
cd "$(dirname "$0")"

if [ ! -f agent9-v0.2.0.qcow2 ]; then
  echo "error: agent9-v0.2.0.qcow2 not found in $(pwd)"
  echo "download it from https://github.com/Alino/agent9/releases"
  exit 1
fi

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
  echo "error: qemu not installed."
  echo "install with: sudo apt install qemu-system-x86  (Debian/Ubuntu)"
  echo "          or: sudo dnf install qemu-system-x86  (Fedora)"
  exit 1
fi

ACCEL=""
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
  ACCEL="-accel kvm -cpu host"
else
  echo "note: KVM unavailable, falling back to TCG. To enable KVM:"
  echo "  sudo usermod -aG kvm \$USER  (then log out + back in)"
  ACCEL="-cpu max"
fi

exec qemu-system-x86_64 \
  -name agent9 \
  -m 2048 \
  -smp 2 \
  $ACCEL \
  -drive file=agent9-v0.2.0.qcow2,if=virtio,format=qcow2 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::1717-:17010,hostfwd=tcp::1564-:564,hostfwd=tcp::53692-:53692,hostfwd=tcp::1455-:1455 \
  -device virtio-net-pci,netdev=net0 \
  -device virtio-rng-pci \
  -usb \
  -device usb-tablet \
  -display "${DISPLAY_BACKEND:-gtk}"
