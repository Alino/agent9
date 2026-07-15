#!/bin/bash
# build-openssl.sh — publish OpenSSL 3.0.17 into the ladybird9 sysroot (tier-2).
#
# Not compiled here: ssl9/ already cross-builds OpenSSL for 9front via cc9
# (see ssl9/build.py; TLS 1.3 proven live on the box). This recipe only copies
# ssl9's static libs + headers into the sysroot so find_package(OpenSSL) and
# curl's -DCURL_USE_OPENSSL=ON resolve from _out/deps like every other dep.
#
# Source of truth: ssl9/vendor/openssl.tar.gz -> ssl9/vendor/openssl-3.0.17
# (in-tree Configure run, so the generated headers opensslconf.h /
# configuration.h already sit in vendor include/openssl).
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
SYS="$A9/ladybird9/_out/deps"
SSL="$A9/ssl9"

test -f "$SSL/_out/libssl.a" -a -f "$SSL/_out/libcrypto.a" || {
  echo "ssl9 libs missing — run ssl9/build.py first" >&2; exit 1; }

mkdir -p "$SYS/lib" "$SYS/include/openssl"
cp "$SSL/_out/libssl.a" "$SSL/_out/libcrypto.a" "$SYS/lib/"
# *.h only (skip the .h.in templates); generated headers are in-tree already.
cp "$SSL"/vendor/openssl-3.0.17/include/openssl/*.h "$SYS/include/openssl/"
echo "openssl 3.0.17 published to $SYS"
