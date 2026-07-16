#!/bin/bash
# build-libjxl.sh — libjxl 0.11.1 (vcpkg pin) -> ladybird9 sysroot.
# Source is a --recurse-submodules clone (the archive tarball's third_party/*
# are empty submodule placeholders; skcms is needed and only comes that way):
#   git clone --depth 1 -b v0.11.1 --recurse-submodules --shallow-submodules \
#     https://github.com/libjxl/libjxl vendor/libjxl
# System brotli + highway (already in the sysroot); skcms bundled; decoder path
# only (no tools/tests/sjpeg/plugins). Installs libjxl.pc so PLAN9's
# pkg_check_modules(Jxl ... libjxl) resolves.
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
LB9="$A9/ladybird9"; SYS="$LB9/_out/deps"; SRC="$LB9/vendor/libjxl"
if [ ! -f "$SRC/third_party/skcms/skcms.cc" ]; then
  echo "libjxl source missing skcms submodule — clone with --recurse-submodules first:" >&2
  echo "  git clone --depth 1 -b v0.11.1 --recurse-submodules --shallow-submodules https://github.com/libjxl/libjxl $SRC" >&2
  exit 1
fi
B="$LB9/_out/build-deps/libjxl"
cmake -G Ninja -S "$SRC" -B "$B" \
  -DCMAKE_TOOLCHAIN_FILE="$LB9/host/toolchain.cmake" \
  -DCMAKE_PROJECT_INCLUDE= \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX="$SYS" -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_C_FLAGS="-DSKCMS_PORTABLE=1" \
  -DCMAKE_CXX_FLAGS="-DSKCMS_PORTABLE=1" \
  -DBUILD_TESTING=OFF \
  -DJPEGXL_ENABLE_TOOLS=OFF -DJPEGXL_ENABLE_BENCHMARK=OFF \
  -DJPEGXL_ENABLE_EXAMPLES=OFF -DJPEGXL_ENABLE_MANPAGES=OFF \
  -DJPEGXL_ENABLE_DOXYGEN=OFF -DJPEGXL_ENABLE_JNI=OFF \
  -DJPEGXL_ENABLE_SJPEG=OFF -DJPEGXL_ENABLE_OPENEXR=OFF \
  -DJPEGXL_ENABLE_PLUGINS=OFF -DJPEGXL_ENABLE_DEVTOOLS=OFF \
  -DJPEGXL_ENABLE_TESTS=OFF \
  -DJPEGXL_ENABLE_JPEGLI=OFF -DJPEGXL_ENABLE_JPEGLI_LIBJPEG=OFF \
  -DJPEGXL_INSTALL_JPEGLI_LIBJPEG=OFF -DJPEGXL_ENABLE_TRANSCODE_JPEG=OFF \
  -DJPEGXL_ENABLE_SKCMS=ON -DJPEGXL_BUNDLE_SKCMS=ON \
  -DJPEGXL_FORCE_SYSTEM_BROTLI=ON -DJPEGXL_FORCE_SYSTEM_HWY=ON \
  -DJPEGXL_STATIC=OFF -DJPEGXL_WARNINGS_AS_ERRORS=OFF
ninja -C "$B" install
echo "libjxl installed -> $SYS"
