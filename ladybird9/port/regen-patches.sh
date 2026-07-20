#!/bin/sh
# regen-patches.sh — regenerate the plan9 patch series from the current vendor
# tree state (pin HEAD + applied patches + edits). Run from anywhere.
#
# The vendor tree is the pin (detached HEAD) with the series applied via
# `git apply --index`, so `git diff HEAD -- <paths>` reproduces each patch,
# new files included. Only the patches whose files changed are rewritten.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)          # ladybird9/port
PATCHES="$HERE/patches"
V="$HERE/../vendor/ladybird"
cd "$V"

gen() { # gen <outfile> <path...>
  out="$1"; shift
  git diff HEAD -- "$@" > "$PATCHES/$out"
  echo "wrote $out ($(grep -c '^diff --git' "$PATCHES/$out") files)"
}

gen 0003-libcore-plan9.patch \
  AK/Platform.h AK/StackInfo.cpp AK/Random.cpp \
  Libraries/LibCore/Resource.h Libraries/LibCore/Socket.cpp \
  Libraries/LibCore/StandardPaths.cpp Libraries/LibCore/System.cpp \
  Libraries/LibCore/System.h Libraries/LibCore/SystemServerTakeover.cpp \
  Libraries/LibCore/CMakeLists.txt \
  Libraries/LibDatabase/Database.cpp \
  Libraries/LibGC/BlockAllocator.cpp Libraries/LibGC/PrimitiveStorage.h

gen 0004-buildsystem-plan9.patch \
  Libraries/LibImageDecoders/CMakeLists.txt Libraries/LibIPC/CMakeLists.txt \
  Libraries/LibJS/CMakeLists.txt Libraries/LibMedia/CMakeLists.txt \
  Libraries/LibSandbox/CMakeLists.txt Meta/CMake/audio.cmake \
  Meta/CMake/check_for_dependencies.cmake \
  Tests/LibIPC/CMakeLists.txt UI/cmake/GUIFramework.cmake \
  UI/cmake/ResourceFiles.cmake Utilities/CMakeLists.txt

gen 0005-libipc-transport-plan9.patch \
  Libraries/LibIPC/Forward.h Libraries/LibIPC/Transport.h \
  Libraries/LibIPC/TransportPlan9.cpp Libraries/LibIPC/TransportPlan9.h

gen 0007-libweb-services-plan9.patch \
  Libraries/LibGfx/Font/FontDatabase.cpp Libraries/LibGfx/Font/TypefaceSkia.cpp \
  Libraries/LibGfx/ImageFormats/ImageDecoder.cpp Libraries/LibTLS/TLSv12.cpp \
  Libraries/LibWasm/AbstractMachine/BytecodeInterpreter.cpp Libraries/LibWasm/CMakeLists.txt \
  Libraries/LibWeb/Loader/UserAgent.h Libraries/LibWeb/Platform/FontPlugin.cpp \
  Services/Compositor/CMakeLists.txt Services/Compositor/main.cpp \
  Services/Compositor/OpenGLContext.cpp \
  Services/ImageDecoder/CMakeLists.txt Services/RequestServer/CMakeLists.txt \
  Services/RequestServer/main.cpp Services/WebContent/CMakeLists.txt \
  Services/WebWorker/CMakeLists.txt

# 0008 = new plan9 sources: UI/Plan9/* + the LibMedia stub bodies (#13: they were
# referenced by 0004's CMakeLists but their source was in no patch).
gen 0008-new-plan9-sources.patch \
  UI/Plan9 \
  Libraries/LibMedia/Audio/PlaybackStreamPlan9.cpp \
  Libraries/LibMedia/Audio/PlaybackStreamPlan9.h \
  Libraries/LibMedia/Codecs/VorbisStubPlan9.cpp \
  Libraries/LibMedia/FFmpeg/FFmpegStubPlan9.cpp

# 0009 = test-web harness (the upstream LibWeb Text/Layout runner): a PLAN9 arm
# for its per-platform screenshot-expectation selector so the TU compiles.
gen 0009-test-web-plan9.patch \
  Tests/LibWeb/test-web/Collection.cpp

# 0010 = skip embedded-ICC-profile parsing on PLAN9 (fall back to sRGB). Skia's
# skcms_Parse overflows a ~1KB stack frame when built by cc9, killing the
# ImageDecoder on every ICC-profiled image; a cc9 codegen bug in third-party
# SIMD code (the same skcms passes on macOS). Kept separate from 0007 so it does
# not entangle with concurrent LibWeb work in that patch.
gen 0010-plan9-skip-icc-colorprofile.patch \
  Libraries/LibGfx/ImageFormats/ImageDecoder.cpp

# 0011 = cap curl concurrent connections on PLAN9. Each socket fd costs ~2 Plan 9
# procs (cc9's poll layer: a reader+writer thread per fd, since Plan 9 has no
# non-blocking I/O). A heavy page's hundreds of connections spawn hundreds of
# procs and the memory pressure has crashed gefs (the 9front fs). Separate patch
# so it does not entangle with concurrent LibWeb work.
gen 0011-plan9-cap-curl-connections.patch \
  Services/RequestServer/ConnectionFromClient.cpp

echo "done."
