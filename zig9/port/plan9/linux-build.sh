#!/bin/sh
# linux-build.sh — rebuild the patched Zig compiler inside the Linux container
# (escapes the macOS-26-SDK link failure that blocks `zig build` on the host) and
# optionally cross-compile a test to x86_64-plan9 with the freshly built compiler.
#
#   sh linux-build.sh build            # (re)build /tmp/zig-out/bin/zig in the container
#   sh linux-build.sh cc SRC OUT       # cross-compile SRC -> OUT (plan9 a.out) with it
#
# The container "zig9build" must be running with the repo mounted at /work
# (see port/plan9/README.md "Path forward"). Output a.outs land in the mounted
# tree so they can be delivered from the host with cc9/host/deliver.py.
set -e
C=zig9build
case "${1:-build}" in
build)
	docker exec "$C" sh -lc '
		cd /work/zig9/vendor/zig &&
		/opt/zig-host/zig build -Denable-llvm=false \
			--cache-dir /tmp/zc --global-cache-dir /tmp/zgc --prefix /tmp/zig-out &&
		/tmp/zig-out/bin/zig version'
	;;
cc)
	src="$2"; out="$3"
	docker exec "$C" sh -lc "
		/tmp/zig-out/bin/zig build-exe '$src' \
			--zig-lib-dir /work/zig9/vendor/zig/lib \
			-target x86_64-plan9 -mcpu=x86_64_v2 -OReleaseSmall -femit-bin='$out'"
	;;
esac
