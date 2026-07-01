#!/bin/sh
# build-host-refs.sh [NAME...] — produce golden PPMs by compiling each corpus
# program in the linux/amd64 container against the SAME Mesa 24.0.9 softpipe we
# built (gl9/build-gen), and running it. Same source cc9 builds for 9front, so the
# only pixel difference vs the target is openlibm-vs-host-libm rounding.
#
#   sh host/build-host-refs.sh                 # all corpus programs
#   sh host/build-host-refs.sh 02_triangle     # one
set -e
HERE=$(cd "$(dirname "$0")" && pwd); GL9=$(dirname "$HERE"); REPO=$(dirname "$GL9")
IMG=gl9build:bookworm; PLAT=linux/amd64
mkdir -p "$GL9/test/goldens"
progs="$*"
[ -n "$progs" ] || progs="01_clear_color 02_triangle 03_shaded_triangle 04_textured_quad 05_instanced_quads 06_depth_blend"

for name in $progs; do
	docker run --rm --platform=$PLAT -v "$REPO":/work -e HOME=/tmp "$IMG" sh -lc '
		set -e
		G=/work/gl9; n='"$name"'
		OSDIR=$(dirname $(find $G/build-gen -name "libOSMesa.so" | head -1))
		GLAPIDIR=$(dirname $(find $G/build-gen -name "libglapi.so" | head -1))
		LIBS=$(find $G/build-gen \( -name "*.so" -o -name "*.so.*" \) -exec dirname {} \; | sort -u | paste -sd:)
		gcc -O2 -I $G/vendor/mesa/include -I $G/test/corpus -DGL_GLEXT_PROTOTYPES \
			$G/test/corpus/$n.c -L $OSDIR -L $GLAPIDIR -lOSMesa -lglapi \
			-Wl,-rpath,$LIBS -o /tmp/$n
		LD_LIBRARY_PATH=$LIBS /tmp/$n $G/test/goldens/$n.ppm
	'
	echo "golden -> test/goldens/$name.ppm"
done
