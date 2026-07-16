#!/bin/bash
# build-ffmpeg.sh — FFmpeg 7.1.1 (vcpkg pin), 4 libs -> ladybird9 sysroot.
# avcodec/avformat/avutil/swresample only (what LibMedia links), C-only:
# --disable-x86asm --disable-inline-asm (FFmpeg's NASM/GAS can't go through
# cc9/LLVM->a.out; the C fallbacks decode, slower). The hostile part is
# FFmpeg's configure probing under cc9 — cross mode compiles+links but never
# runs test programs. Installs libav*.pc so LibMedia's PkgConfig::AV* resolve.
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
LB9="$A9/ladybird9"; SYS="$LB9/_out/deps"; SRC="$LB9/vendor/ffmpeg"
TB="$LB9/vendor/_tarballs/ffmpeg-7.1.1.tar.xz"
URL="https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
CC9CC="$A9/servo9/host/cc9-cc"
if [ ! -f "$SRC/configure" ]; then
  mkdir -p "${TB%/*}" "$SRC"
  [ -f "$TB" ] || curl -fsSL "$URL" -o "$TB"
  tar -xf "$TB" -C "$SRC" --strip-components=1
fi
B="$LB9/_out/build-deps/ffmpeg"
rm -rf "$B"; mkdir -p "$B"
cd "$B"
"$SRC/configure" \
  --prefix="$SYS" --libdir="$SYS/lib" --incdir="$SYS/include" \
  --enable-cross-compile --arch=x86_64 --target-os=none \
  --cc="$CC9CC" --ar="$LLVM/llvm-ar" --ranlib="$LLVM/llvm-ranlib" \
  --nm="$LLVM/llvm-nm" --strip="$LLVM/llvm-strip" \
  --pkg-config=pkg-config \
  --enable-static --disable-shared --enable-pic \
  --disable-x86asm --disable-inline-asm --disable-asm \
  --disable-programs --disable-doc --disable-network \
  --disable-avdevice --disable-avfilter --disable-swscale \
  --disable-postproc --disable-encoders --disable-muxers \
  --disable-bsfs --disable-devices --disable-filters \
  --enable-decoders --enable-demuxers --enable-parsers --enable-protocols \
  --disable-debug
make -j"$(sysctl -n hw.ncpu)"
# install-libs/-headers only: plain `make install` also builds install-examples
# / install-data which we don't want (and which write outside the sysroot).
make install-libs install-headers
# FFmpeg's .pc carries -pthread in Libs; that's a compiler-driver flag ld.lld
# (via cc9-link) rejects. cc9 links pthreads as -lpthreads elsewhere, so drop it.
for pc in libavcodec libavformat libavutil libswresample; do
  [ -f "$SYS/lib/pkgconfig/$pc.pc" ] && sed -i.bak 's/ -pthread//g' "$SYS/lib/pkgconfig/$pc.pc" && rm -f "$SYS/lib/pkgconfig/$pc.pc.bak"
done
echo "ffmpeg installed -> $SYS"
