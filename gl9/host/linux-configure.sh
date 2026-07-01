#!/bin/sh
# linux-configure.sh — run Mesa's meson+ninja inside a linux/amd64 container to
# (a) materialize Mesa's generated C sources, (b) emit compile_commands.json (the
# bridge host/harvest.py reads), and (c) build a native amd64 softpipe+OSMesa we
# reuse as the parity golden oracle. This does NOT target 9front — cc9 does that
# on the host from the harvested file set (host/build-gl9.py).
#
#   sh host/linux-configure.sh            # image + meson setup + ninja -j4
#   sh host/linux-configure.sh reconfigure  # wipe build-gen and re-setup
#
# Requires: docker. The image is built --platform=linux/amd64 so meson emits
# x86-64 arch defines/dispatch matching cc9's target (NOT the host arm64).
# NB: emulated amd64 ninja at -j$(nproc) OOM'd Docker Desktop's VM; -j4 is safe.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)          # gl9/host
GL9=$(dirname "$HERE")                        # gl9
REPO=$(dirname "$GL9")                         # agent9
IMG=gl9build:bookworm
PLAT=linux/amd64
BG="$GL9/build-gen"

[ -d "$GL9/vendor/mesa" ] || { echo "run gl9/fetch.sh first"; exit 1; }

echo "== build container image ($PLAT) =="
docker build --platform=$PLAT -t "$IMG" -f "$HERE/Dockerfile" "$HERE"

if [ "${1:-}" = reconfigure ]; then rm -rf "$BG"; fi

drun() {
	docker run --rm --platform=$PLAT --user "$(id -u):$(id -g)" -e HOME=/tmp \
		-v "$REPO":/work "$IMG" sh -lc "$1"
}

# softpipe is selected via the `swrast` gallium driver (with llvm=disabled it is
# softpipe; llvmpipe when enabled). EGL is added in Phase 4; OSMesa is the wedge.
if [ ! -f "$BG/compile_commands.json" ]; then
	echo "== meson setup =="
	drun 'cd /work/gl9/vendor/mesa && meson setup /work/gl9/build-gen . \
		-Dgallium-drivers=swrast -Dvulkan-drivers= -Dllvm=disabled \
		-Dglx=disabled -Degl=disabled -Dgbm=disabled \
		-Dopengl=true -Dgles1=disabled -Dgles2=enabled -Dosmesa=true \
		-Dshared-glapi=enabled -Dplatforms= -Dshader-cache=disabled \
		-Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled \
		-Dgallium-nine=false -Dgallium-rusticl=false -Dgallium-opencl=disabled \
		-Dvideo-codecs= -Dbuild-tests=false \
		-Dbuildtype=release -Db_ndebug=true -Dc_std=c11 -Dcpp_std=c++17'
fi

echo "== ninja -j4 (materialize generated sources + oracle) =="
drun 'cd /work/gl9/build-gen && ninja -j4'

echo "done. next: python3 host/harvest.py && python3 host/build-gl9.py enumerate"
