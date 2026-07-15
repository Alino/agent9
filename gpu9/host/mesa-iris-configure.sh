#!/bin/sh
# mesa-iris-configure.sh — configure Mesa with the IRIS driver (Intel Gen8+ HW)
# so it emits iris/intel sources + compile_commands for the cc9 cross-build.
# Same bridge as gl9/llvm9: a linux/amd64 container runs meson; cc9 does the
# actual 9front build from the harvested file set.
#
# iris needs libdrm at CONFIGURE time (headers + pkg-config). We install it in
# the container purely to satisfy meson — the built code never calls into libdrm
# on 9front: iris's whole kernel contract is intel_gem.h's intel_ioctl(), which
# is a plain POSIX ioctl() that cc9's runtime will implement in-process.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$(dirname "$HERE")")"
IMG=irisbuild:bookworm
PLAT=linux/amd64
BG=/work/gl9/build-gen-iris

docker build --platform=$PLAT -t "$IMG" -f "$HERE/Dockerfile" "$HERE"

drun() {
  docker run --rm --platform=$PLAT --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$REPO":/work "$IMG" sh -c "$1"
}

if [ ! -f "$REPO/gl9/build-gen-iris/compile_commands.json" ]; then
  echo "== meson setup (gallium-drivers=iris) =="
  drun "cd /work/gl9/vendor/mesa && meson setup $BG . \
    -Dgallium-drivers=iris,swrast -Dvulkan-drivers= -Dllvm=disabled \
    -Dglx=disabled -Degl=disabled -Dgbm=disabled \
    -Dopengl=true -Dgles1=disabled -Dgles2=enabled -Dosmesa=true \
    -Dshared-glapi=enabled -Dplatforms= -Dshader-cache=disabled \
    -Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled \
    -Dgallium-nine=false -Dgallium-rusticl=false -Dgallium-opencl=disabled \
    -Dvideo-codecs= -Dbuild-tests=false \
    -Dbuildtype=release -Db_ndebug=true -Dc_std=c11 -Dcpp_std=c++17"
fi

echo "== ninja: materialize generated sources (ignore native link failures) =="
drun "cd $BG && ninja -j4 || true"

# driconf_static.h — the built-in driconf option DEFAULTS, baked into a header.
# meson only generates this for a WITH_XMLCONFIG=0 build; ours configures =1
# (the container HAS expat), so ninja never emits it. cc9 has no expat, so the
# cc9 build drops -DWITH_XMLCONFIG and takes Mesa's static path — which needs
# this header. Generate it unconditionally; it costs nothing when unused.
#
# NB argument order is `drirc... header` (meson passes @INPUT@ @OUTPUT@).
# Reversing it makes the generator TRUNCATE 00-mesa-defaults.conf to 0 bytes,
# and vendor/ is .gitignore'd, so there is nothing to `git checkout` back.
echo "== generate driconf_static.h (defaults for the no-expat build) =="
drun "cd /work/gl9/vendor/mesa && python3 src/util/driconf_static.py \
  src/util/00-mesa-defaults.conf $BG/src/util/driconf_static.h"

echo "done -> gl9/build-gen-iris/compile_commands.json"
