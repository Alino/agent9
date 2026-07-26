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
  Libraries/LibCore/Process.cpp \
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
  Services/Compositor/OpenGLContext.cpp Services/Compositor/OpenGLContext.h \
  Services/ImageDecoder/CMakeLists.txt Services/RequestServer/CMakeLists.txt \
  Services/RequestServer/main.cpp Services/WebContent/CMakeLists.txt \
  Services/WebWorker/CMakeLists.txt Services/WebWorker/main.cpp

# 0008 = new plan9 sources: UI/Plan9/* + the LibMedia stub bodies (#13: they were
# referenced by 0004's CMakeLists but their source was in no patch).
gen 0008-new-plan9-sources.patch \
  UI/Plan9 \
  Libraries/LibMedia/Audio/PlaybackStreamPlan9.cpp \
  Libraries/LibMedia/Audio/PlaybackStreamPlan9.h \
  Libraries/LibMedia/Codecs/VorbisStubPlan9.cpp \
  Libraries/LibMedia/FFmpeg/FFmpegStubPlan9.cpp

# 0006 = the utilities/tests arm: js(1) and the LibIPC connection test, plus the
# LibWeb HTTP fixture server (9front has no loopback by default, so it binds the
# box's real address).
gen 0006-utilities-tests-plan9.patch \
  Tests/LibIPC/TestConnection.cpp Utilities/js.cpp \
  Tests/LibWeb/Fixtures/http-test-server.py

# 0009 = test-web harness (the upstream LibWeb Text/Layout runner): a PLAN9 arm
# for its per-platform screenshot-expectation selector so the TU compiles.
gen 0009-test-web-plan9.patch \
  Tests/LibWeb/test-web/Collection.cpp Tests/LibWeb/test-web/main.cpp


# 0011 = the RequestServer connection's PLAN9 arms:
#  - cap curl concurrent connections. Each socket fd costs ~2 Plan 9 procs (cc9's
#    poll layer: a reader+writer thread per fd, since Plan 9 has no non-blocking
#    I/O). A heavy page's hundreds of connections spawn hundreds of procs and the
#    memory pressure has crashed gefs (the 9front fs).
#  - a repeating progress timer that re-drives curl and each request's queued
#    writes, because the socket Core::Notifiers proved unreliable on cc9 and a
#    missed one stalls a multi-MB transfer forever.
gen 0011-plan9-cap-curl-connections.patch \
  Services/RequestServer/ConnectionFromClient.cpp \
  Services/RequestServer/ConnectionFromClient.h

# 0012 = MediaSource Extensions session APIs (upstream-shaped MSE work). YouTube's
# modern player is MSE-only: without SourceBuffer/MediaSource advertising a
# supported type it refuses to play at all ("Your browser can't play this video").
gen 0012-mse-session-apis.patch \
  Libraries/LibMedia/CodedFrame.h \
  Libraries/LibMedia/PlaybackManager.cpp Libraries/LibMedia/PlaybackManager.h \
  Libraries/LibWeb/HTML/AudioTrackList.cpp Libraries/LibWeb/HTML/AudioTrackList.h \
  Libraries/LibWeb/HTML/TextTrackList.cpp Libraries/LibWeb/HTML/TextTrackList.h \
  Libraries/LibWeb/HTML/VideoTrackList.cpp Libraries/LibWeb/HTML/VideoTrackList.h \
  Libraries/LibWeb/MediaSourceExtensions \
  Tests/LibWeb/TestConfig.ini \
  Tests/LibWeb/Text/expected/HTML/media-source-remove.txt \
  Tests/LibWeb/Text/input/HTML/media-source-remove.html

# 0013 = response-body delivery on PLAN9. RequestServer downloaded fine but
# WebContent never drained the response pipe, so pages rendered blank: the pipe fd
# arrives BLOCKING (fds cross by name through /srv, not SCM_RIGHTS), and the
# readable/writable Core::Notifiers on it stop firing mid-body. Fixes: force the
# receiving fd non-blocking, only close the read notifier once the request is
# actually done (a drained non-blocking pipe reports is_eof()), re-drive both
# drains on timers, and abort transfers that connect and then go silent.
gen 0013-plan9-response-pipe-delivery.patch \
  Libraries/LibRequests/Request.cpp Libraries/LibRequests/Request.h \
  Services/RequestServer/Request.cpp Services/RequestServer/Request.h

# 0014 = what youtube.com's player needs beyond MSE. Both changes are symmetric
# (they apply to the host parity build too), so the byte-identical comparison
# still holds:
#  - Navigator.getBattery() is hidden. LibWeb has no BatteryManager, so the method
#    exists but ALWAYS rejects; YouTube calls it without a .catch(), and the
#    unhandled rejection kills the player init chain. Firefox ships no Battery API
#    at all and sites cope. Restore when BatteryManager is implemented.
#  - autoplay also starts at HAVE_FUTURE_DATA. The spec puts the autoplay steps
#    only in the HAVE_ENOUGH_DATA branch, but an MSE source that buffers just
#    ahead of the playhead never reaches "5s ahead" or EOF while paused, so
#    autoplay would never fire and the video sits on its first frame.
#  - LB9_YT_CONSENT seeds the consent-acknowledgement cookies a user click would
#    set (headless starts with an empty jar and youtube.com shows its interstitial
#    instead of the watch page). Opt-in via the env var; PLAN9-guarded.
gen 0014-plan9-youtube-player.patch \
  Libraries/LibWeb/HTML/Navigator.idl \
  Libraries/LibWeb/HTML/HTMLMediaElement.cpp \
  Libraries/LibWebView/Application.cpp

# 0015 = keep the HTTP disk cache when the platform cannot report free space.
# Plan 9 has NO way to answer statvfs (no statfs syscall, stat(5)'s Rstat has no
# free-space field, 9front ships no df(1) for that reason), so cc9's statvfs
# fails with ENOSYS by design rather than fabricating a number. CacheIndex::create
# TRY'd it, so that ENOSYS killed DiskCache::create outright and every response
# was refetched forever -- which also kept the JS BYTECODE cache dead, since it
# is stored in the same cache. Falling back to a fixed budget is not a claim
# about the disk; it is a policy choice about how much we will use, which is all
# free_disk_space feeds.
gen 0015-plan9-disk-cache-without-statvfs.patch \
  Libraries/LibHTTP/Cache/CacheIndex.cpp

echo "done."
