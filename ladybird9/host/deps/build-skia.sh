#!/bin/bash
# build-skia.sh — Skia chrome/m148 (Ladybird vcpkg pin "148#0") -> ladybird9 sysroot.
# Repo:   https://github.com/google/skia (mirror of skia.googlesource.com/skia)
# Branch: chrome/m148
# SHA:    46f2e16555cac1211f4087cf24728fd741ac6495  (pinned below)
#
# CPU raster only: no GL/Vulkan/Metal/D3D/ANGLE, no graphite. Ganesh IS
# compiled (with zero GPU backends) because Ladybird's LibGfx references
# ganesh symbols behind *runtime* — not preprocessor — guards:
#   PaintingSurface.cpp:  SkSurfaces::RenderTarget(...)   [if (context) path]
#   SkiaBackendContext.cpp: GrDirectContext::performDeferredCleanup(...)
# A ganesh-free build (skia_enable_ganesh=false) compiles fine and is ~7MB
# smaller, but leaves those undefined at Ladybird link time. Flip GANESH=false
# below if Ladybird ever compile-guards its GPU paths.
#
# Codecs (png/jpeg/webp/gif...) are all OFF: Ladybird decodes images itself
# (LibGfx + wuffs); skia is only the raster/canvas/font engine. Fonts:
# FreeType + custom-directory/custom-empty fontmgrs (no fontconfig on 9front).
# skcms is built into libskia.a AND shipped as libskcms.a (skia.pc links both,
# mirroring Ladybird's own Meta/CMake/flatpak/skia recipe).
#
# Discovery: Ladybird's check_for_dependencies.cmake falls back to
# pkg_check_modules(skia skia=148) when the vcpkg unofficial-skia CONFIG
# package is absent -> lib/pkgconfig/skia.pc with Version: 148 exactly.
# Header layout mirrors the flatpak recipe: include/skia/<core|effects|...>
# (skia's include/ contents), include/skia/modules/skcms/..., and the
# installed headers' `#include "include/...` rewritten to `#include "...`.
#
# GN args vs the stock linux build, and why:
#   cc/cxx = cc9 wrappers  — GN's is_clang sniff runs `cc9-cc --version`,
#                            which execs clang; detection Just Works.
#   ar = llvm-ar           — archives ELF objects on a mac host.
#   -DSK_BUILD_FOR_UNIX    — cc9's --target=x86_64-unknown-none defines no
#                            platform macro; SkFeatures.h else-defaults to
#                            SK_BUILD_FOR_MAC (=> dispatch/dispatch.h). The
#                            same define is carried in skia.pc Cflags so
#                            consumer TUs see the same platform as the lib.
#   -fno-pic               — the wrapper's -fno-pic comes first; skia's
#                            default config adds -fPIC after it. extra_cflags
#                            are appended last, so this re-overrides.
#   skia_use_perfetto=false — linux default is true; needs third_party
#                            externals + Linux tracing fds. Tracing is off
#                            (SK_DISABLE_TRACING) in official builds anyway.
#   system freetype/zlib   — from the ladybird9 sysroot (build-freetype.sh /
#                            build-zlib.sh). zlib is actually inert here
#                            (only pdf/png-encode pull it; both off).
# No DEPS sync needed: with everything above disabled, m148 builds from the
# bare checkout (skcms is in-tree at modules/skcms; gn via bin/fetch-gn).
# AVX/AVX512 SkOpts TUs are compiled but runtime-CPUID-dispatched; fine.
set -euo pipefail
A9="$(cd "$(dirname "$0")/../../.." && pwd)"
LB9="$A9/ladybird9"; SYS="$LB9/_out/deps"; SRC="$LB9/vendor/skia"
SHA=46f2e16555cac1211f4087cf24728fd741ac6495
GANESH=${SKIA_GANESH:-true}
OUT="$SRC/out/plan9-ganesh"; [ "$GANESH" = false ] && OUT="$SRC/out/plan9"

if [ ! -d "$SRC/.git" ]; then
  git clone --depth 1 --branch chrome/m148 https://github.com/google/skia.git "$SRC"
fi
got="$(git -C "$SRC" rev-parse HEAD)"
if [ "$got" != "$SHA" ]; then
  git -C "$SRC" fetch --depth 1 origin "$SHA" && git -C "$SRC" checkout "$SHA" \
    || { echo "skia checkout is at $got, want $SHA" >&2; exit 1; }
fi
[ -x "$SRC/bin/gn" ] || (cd "$SRC" && python3 bin/fetch-gn)

FT_INC="$SYS/include/freetype2"
[ -d "$FT_INC" ] || { echo "need $FT_INC — run build-freetype.sh first" >&2; exit 1; }

mkdir -p "$OUT"
cat > "$OUT/args.gn" <<EOF
is_official_build = true
is_debug = false
is_component_build = false
cc = "$A9/servo9/host/cc9-cc"
cxx = "$A9/servo9/host/cc9-c++"
ar = "/opt/homebrew/opt/llvm/bin/llvm-ar"
target_os = "linux"
target_cpu = "x64"
skia_use_gl = false
skia_use_vulkan = false
skia_use_metal = false
skia_use_direct3d = false
skia_use_angle = false
skia_enable_ganesh = $GANESH
skia_enable_graphite = false
skia_use_freetype = true
skia_use_system_freetype2 = true
skia_system_freetype2_include_path = "$FT_INC"
skia_use_fontconfig = false
skia_use_harfbuzz = false
skia_use_icu = false
skia_use_perfetto = false
skia_enable_fontmgr_custom_directory = true
skia_enable_fontmgr_custom_empty = true
skia_use_libpng_decode = false
skia_use_libpng_encode = false
skia_use_libjpeg_turbo_decode = false
skia_use_libjpeg_turbo_encode = false
skia_use_libwebp_decode = false
skia_use_libwebp_encode = false
skia_use_zlib = true
skia_use_system_zlib = true
skia_enable_pdf = false
skia_enable_skottie = false
skia_use_expat = false
skia_use_dng_sdk = false
skia_use_piex = false
skia_use_wuffs = false
extra_cflags = [
  "-fno-pic",
  "-DSK_BUILD_FOR_UNIX",
  "-I$SYS/include",
]
extra_cflags_cc = [ "-DSKCMS_DLL" ]
EOF
(cd "$SRC" && ./bin/gn gen "$OUT")
ninja -C "$OUT" skia

# skia's :pathops source_set is NOT part of libskia.a (Ladybird's Compositor
# links it separately). It used to be hand-built into the CMake build tree,
# where a clean wiped it and left an unbuildable tree; build it from the same
# GN config (so the flags/ABI match libskia.a exactly) and install it into the
# sysroot alongside the other archives.
ninja -C "$OUT" pathops
rm -f "$OUT/libskiapathops.a"
/opt/homebrew/opt/llvm/bin/llvm-ar crs "$OUT/libskiapathops.a" "$OUT"/obj/src/pathops/*.o

# --- install: headers (flatpak layout), libs, skia.pc -----------------------
INC="$SYS/include/skia"
rm -rf "$INC"; mkdir -p "$INC/modules/skcms" "$SYS/lib/pkgconfig"
(cd "$SRC/include" && find . -name '*.h' -exec rsync -R {} "$INC/" \;)
(cd "$SRC" && find modules/skcms -name '*.h' -exec rsync -R {} "$INC/" \;)
# Installed layout drops the include/ prefix; rewrite self-references to match.
grep -rl '#include "include/' "$INC" | while read -r f; do
  sed -i '' 's|#include "include/|#include "|g' "$f"
done
install -m644 "$OUT/libskia.a" "$OUT/libskcms.a" "$OUT/libskiapathops.a" "$SYS/lib/"

cat > "$SYS/lib/pkgconfig/skia.pc" <<EOF
prefix=$SYS
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include/skia

Name: skia
Description: 2D graphics library (CPU raster; ganesh=$GANESH with no GPU backends)
URL: https://skia.org/
Version: 148
Libs: -L\${libdir} -lskia -lskcms
Cflags: -I\${includedir} -DSK_BUILD_FOR_UNIX -DSK_R32_SHIFT=16 -DSK_GANESH -DSK_CODEC_DECODES_BMP -DSK_CODEC_DECODES_WBMP -DSK_ENABLE_PRECOMPILE -DSK_DISABLE_TRACING
EOF
# Cflags carries skia's gn "skia_public" defines (gn desc //:skia_public) so
# consumer TUs agree with the lib on header-inline ABI (SK_R32_SHIFT!) and
# platform (SK_BUILD_FOR_UNIX). SKCMS_DLL is added by Ladybird's cmake itself.
# ponytail: drop -DSK_GANESH from Cflags if GANESH=false ever becomes the build.

echo "skia $SHA installed into $SYS (libskia.a $(du -h "$SYS/lib/libskia.a" | cut -f1))"
