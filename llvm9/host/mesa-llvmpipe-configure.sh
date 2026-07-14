#!/bin/sh
# mesa-llvmpipe-configure.sh — reconfigure Mesa's meson with LLVM ENABLED so it
# emits llvmpipe/gallivm sources + compile_commands. Uses the fake llvm-config
# (host/llvm-config) pointing at our cross-built LLVM 22 tree, so gallivm is
# configured against the SAME headers as libllvm9.a. Separate build dir
# (build-gen-llvm) so it doesn't clobber the softpipe build-gen.
#
# We only need meson setup (writes compile_commands) + ninja far enough to
# materialize generated sources — NOT a native llvmpipe link (no linux LLVM libs
# in the container; cc9 does the target build). So ninja failures at the native
# link step are expected and ignored.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)              # llvm9/host
REPO=$(dirname "$(dirname "$HERE")")             # agent9
LLVMSRC="${CC9_LLVMSRC:-$HOME/Projects/llvm-project}"
IMG=gl9build:bookworm                            # already has meson/ninja/mako/flex/bison
PLAT=linux/amd64
BG=/work/gl9/build-gen-llvm

drun() {
  docker run --rm --platform=$PLAT --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$REPO":/work -v "$LLVMSRC":/work-llvm "$IMG" sh -c "$1"
}

if [ ! -f "$REPO/gl9/build-gen-llvm/compile_commands.json" ]; then
  echo "== meson setup (llvm=enabled, llvmpipe) =="
  drun "cd /work/gl9/vendor/mesa && chmod +x /work/llvm9/host/llvm-config && \
    meson setup $BG . --native-file /work/llvm9/host/mesa-native.ini \
    -Dgallium-drivers=swrast -Dvulkan-drivers= -Dllvm=enabled -Dshared-llvm=disabled \
    -Ddraw-use-llvm=true \
    -Dglx=disabled -Degl=disabled -Dgbm=disabled \
    -Dopengl=true -Dgles1=disabled -Dgles2=enabled -Dosmesa=true \
    -Dshared-glapi=enabled -Dplatforms= -Dshader-cache=disabled \
    -Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled \
    -Dgallium-nine=false -Dgallium-rusticl=false -Dgallium-opencl=disabled \
    -Dvideo-codecs= -Dbuild-tests=false \
    -Dbuildtype=release -Db_ndebug=true -Dc_std=c11 -Dcpp_std=c++17 -Dcpp_rtti=false"
fi

echo "== ninja: generate sources only (ignore native-link failures) =="
drun "cd $BG && ninja -j4 || true"
echo "done. compile_commands -> gl9/build-gen-llvm/compile_commands.json"
