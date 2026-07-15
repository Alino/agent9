# Shared plumbing for the ladybird9 dep recipes (build-<dep>.sh).
# Each recipe pins URL + sha256 (gl9/fetch.sh style), vendors the source into
# ladybird9/vendor/<dep>/, builds STATIC with the cc9 toolchain, and installs
# into the ladybird9/_out/deps sysroot that host/toolchain.cmake searches.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)              # ladybird9/host/deps
LB9=$(cd "$HERE/../.." && pwd)                   # ladybird9/
AGENT9=$(cd "$LB9/.." && pwd)
VENDOR="$LB9/vendor"
PREFIX="$LB9/_out/deps"
TOOLCHAIN="$LB9/host/toolchain.cmake"
CC9CC="$AGENT9/servo9/host/cc9-cc"
CC9CXX="$AGENT9/servo9/host/cc9-c++"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
AR="$LLVM/llvm-ar"

mkdir -p "$VENDOR" "$PREFIX/include" "$PREFIX/lib/pkgconfig" "$PREFIX/lib/cmake"

# fetch URL SHA256 DESTDIR — verify the pin, extract (tar.gz/tar.xz/zip), move
# the single extracted top-level dir to DESTDIR. Idempotent: present dir wins.
fetch() {
	url=$1; sha=$2; dest=$3
	if [ -d "$dest" ]; then
		echo "already present: $dest"
		return 0
	fi
	tmp=$(mktemp -d)
	echo "fetching $url"
	curl -fsSL -o "$tmp/a" "$url"
	got=$(shasum -a 256 "$tmp/a" | awk '{print $1}')
	if [ "$got" != "$sha" ]; then
		echo "shasum mismatch for $url" >&2
		echo "  want $sha" >&2
		echo "  got  $got" >&2
		rm -rf "$tmp"; exit 1
	fi
	mkdir "$tmp/x"
	case "$url" in
	*.zip)    unzip -q "$tmp/a" -d "$tmp/x" ;;
	*.tar.xz) tar xJf "$tmp/a" -C "$tmp/x" ;;
	*)        tar xzf "$tmp/a" -C "$tmp/x" ;;
	esac
	inner=$(find "$tmp/x" -maxdepth 1 -mindepth 1 -type d | head -1)
	mv "$inner" "$dest"
	rm -rf "$tmp"
	echo "-> $dest"
}

# cmake_dep SRCDIR [extra -D args...] — configure+build+install a static CMake
# dep against the cc9 cross toolchain into the sysroot. Build tree lives inside
# the vendored source (vendor/ is gitignored).
cmake_dep() {
	src=$1; shift
	cmake -G Ninja -S "$src" -B "$src/build-plan9" \
		-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_INSTALL_LIBDIR=lib \
		-DBUILD_SHARED_LIBS=OFF \
		"$@"
	cmake --build "$src/build-plan9"
	cmake --install "$src/build-plan9"
}
