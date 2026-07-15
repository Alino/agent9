#!/bin/bash
# build-curl.sh — curl 8.20.0 (vcpkg pin) -> ladybird9 sysroot (tier-2).
# URL:    https://curl.se/download/curl-8.20.0.tar.xz
# SHA256: 63fe2dc148ba0ceae89922ef838f7e5c946272c2e78b7c59fab4b79d3ce2b896
#
# Ladybird: find_package(CURL) -> CURL::libcurl from curl's own CURLConfig.
# Explicit toggles instead of trusting configure probes (cross to plan9):
#   CURL_USE_OPENSSL=ON     sysroot OpenSSL 3.0.17 (ssl9)
#   USE_NGHTTP2=ON          HTTP/2 (sysroot libnghttp2)
#   CURL_BROTLI/CURL_ZSTD   content decoding (sysroot)
#   CURL_ZLIB=ON            sysroot libz (tier-1)
#   CURL_USE_LIBPSL=OFF     no libpsl in sysroot yet (Ladybird links its own)
#   USE_UNIX_SOCKETS=OFF    NO AF_UNIX on 9front
#   CURL_DISABLE_WEBSOCKETS=OFF  ws/wss stay on (RequestServer uses them)
#   HTTP/3 off (default: no ngtcp2/quiche/msh3), no ssh/idn/gsasl/ldap/rtmp
#   CA: /sys/lib/tls/ca.pem — 9front's stock CA bundle; CURL_CA_PATH=none so
#       no host path leaks in. CA_FALLBACK off.
#   Threaded resolver stays ON (cc9 has real pthreads).
# Static lib only; no curl tool, docs, tests, examples.
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
LB9="$A9/ladybird9"; SYS="$LB9/_out/deps"; SRC="$LB9/vendor/curl"
TB="$LB9/vendor/_tarballs/curl-8.20.0.tar.xz"
if [ ! -f "$SRC/CMakeLists.txt" ]; then
  mkdir -p "${TB%/*}" "$SRC"
  [ -f "$TB" ] || curl -fsSL https://curl.se/download/curl-8.20.0.tar.xz -o "$TB"
  echo "63fe2dc148ba0ceae89922ef838f7e5c946272c2e78b7c59fab4b79d3ce2b896  $TB" | shasum -a 256 -c -
  tar -xf "$TB" -C "$SRC" --strip-components=1
fi
# One-line vendor patch: curl/curl.h only includes <sys/select.h> on an
# allowlist of platforms; cc9 keeps fd_set there too, so add __plan9__
# (cc9-cc defines it). Fixes the lib build AND every later consumer of the
# installed header (multi.h uses fd_set in curl_multi_fdset's signature).
grep -q '__plan9__' "$SRC/include/curl/curl.h" || \
  sed -i '' 's/defined(__sun__) || defined(__serenity__) || defined(__vxworks__)/defined(__sun__) || defined(__serenity__) || defined(__vxworks__) || defined(__plan9__)/' \
    "$SRC/include/curl/curl.h"
grep -q '__plan9__' "$SRC/include/curl/curl.h" || { echo "curl.h patch failed"; exit 1; }

B="$LB9/_out/build-deps/curl"
cmake -G Ninja -S "$SRC" -B "$B" \
  -DCMAKE_TOOLCHAIN_FILE="$LB9/host/toolchain.cmake" \
  -DCMAKE_PROJECT_INCLUDE= \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
  -DCMAKE_INSTALL_PREFIX="$SYS" \
  -DBUILD_CURL_EXE=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
  -DBUILD_LIBCURL_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
  -DCURL_USE_OPENSSL=ON \
  -DUSE_NGHTTP2=ON \
  -DCURL_BROTLI=ON -DCURL_ZSTD=ON -DCURL_ZLIB=ON \
  -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF \
  -DCURL_USE_GSASL=OFF -DUSE_LIBIDN2=OFF \
  -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON \
  -DUSE_UNIX_SOCKETS=OFF \
  -DCURL_DISABLE_WEBSOCKETS=OFF \
  -DCURL_CA_BUNDLE=/sys/lib/tls/ca.pem -DCURL_CA_PATH=none -DCURL_CA_FALLBACK=OFF \
  -DHAVE_LIBSOCKET=0
# ^ HAVE_LIBSOCKET pre-seeded off: check_library_exists() is compile-only
# under this toolchain (TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY), so curl
# "finds" Solaris libsocket and bakes $<LINK_ONLY:socket> into CURL::libcurl,
# which would make Ladybird's final link demand a nonexistent -lsocket.
ninja -C "$B" install
