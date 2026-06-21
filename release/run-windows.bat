@echo off
REM run-windows.bat — boot agent9 on Windows.
REM
REM Requires: QEMU for Windows (https://www.qemu.org/download/#windows)
REM Adds the install directory to PATH or run from inside it.

setlocal
cd /d "%~dp0"

if not exist agent9-v0.2.0.qcow2 (
  echo error: agent9-v0.2.0.qcow2 not found in %CD%
  echo download it from https://github.com/Alino/agent9/releases
  exit /b 1
)

where qemu-system-x86_64 >nul 2>nul
if errorlevel 1 (
  echo error: qemu not on PATH.
  echo install from https://www.qemu.org/download/#windows
  echo and add it to PATH, or run this script from the QEMU folder.
  exit /b 1
)

REM WHPX is Windows Hypervisor Platform. Falls back to TCG if not present.
set ACCEL=-accel whpx,kernel-irqchip=off -cpu max

qemu-system-x86_64 ^
  -name agent9 ^
  -m 2048 ^
  -smp 2 ^
  %ACCEL% ^
  -drive file=agent9-v0.2.0.qcow2,if=virtio,format=qcow2 ^
  -netdev user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::1717-:17010,hostfwd=tcp::1564-:564,hostfwd=tcp::53692-:53692,hostfwd=tcp::1455-:1455 ^
  -device virtio-net-pci,netdev=net0 ^
  -device virtio-rng-pci ^
  -usb ^
  -device usb-tablet ^
  -display sdl
