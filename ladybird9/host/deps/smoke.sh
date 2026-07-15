#!/bin/sh
# Acceptance check for the tier-1 sysroot deps:
#  1. every member of every installed .a is elf64-x86-64
#  2. one TU per dep compiles with cc9-cc/cc9-c++ against the sysroot
#  3. libtommath resolves via pkg-config the way Ladybird consumes it
. "$(dirname "$0")/common.sh"

OBJDUMP="$LLVM/llvm-objdump"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

echo "== archive architectures =="
for a in "$PREFIX"/lib/*.a; do
	bad=$("$OBJDUMP" -f "$a" 2>&1 | grep 'file format' | grep -cv elf64-x86-64 || true)
	n=$("$OBJDUMP" -f "$a" 2>/dev/null | grep -c 'file format')
	if [ "$bad" -eq 0 ] && [ "$n" -gt 0 ]; then
		echo "ok   $(basename "$a") ($n members)"
	else
		echo "FAIL $(basename "$a") ($bad non-elf64-x86-64 of $n)"; fail=1
	fi
done

echo "== compile smokes =="
smoke() { # smoke NAME COMPILER SRC
	name=$1; comp=$2; src=$3
	if "$comp" -O2 -I"$PREFIX/include" -c "$src" -o "$TMP/$name.o" 2>"$TMP/$name.err"; then
		echo "ok   $name"
	else
		echo "FAIL $name"; sed 's/^/     /' "$TMP/$name.err" | head -8; fail=1
	fi
}

cat > "$TMP/t_fmt.cpp" <<'EOF'
#include <fmt/format.h>
std::string f() { return fmt::format("{}+{}", 1, 2); }
EOF
smoke fmt "$CC9CXX" "$TMP/t_fmt.cpp"

cat > "$TMP/t_simdutf.cpp" <<'EOF'
#include <simdutf.h>
bool f(char const* p, size_t n) { return simdutf::validate_utf8(p, n); }
EOF
smoke simdutf "$CC9CXX" "$TMP/t_simdutf.cpp"

cat > "$TMP/t_simdjson.cpp" <<'EOF'
#include <simdjson.h>
simdjson::dom::parser p;
EOF
smoke simdjson "$CC9CXX" "$TMP/t_simdjson.cpp"

cat > "$TMP/t_sqlite3.c" <<'EOF'
#include <sqlite3.h>
int f(void) { return sqlite3_libversion_number(); }
EOF
smoke sqlite3 "$CC9CC" "$TMP/t_sqlite3.c"

cat > "$TMP/t_zlib.c" <<'EOF'
#include <zlib.h>
unsigned long f(unsigned long n) { return compressBound(n); }
EOF
smoke zlib "$CC9CC" "$TMP/t_zlib.c"

cat > "$TMP/t_mimalloc.cpp" <<'EOF'
#include <mimalloc.h>
void* f() { return mi_heap_malloc(mi_heap_new(), 32); }
void* g() { return mi_malloc_aligned(64, 32); }
EOF
smoke mimalloc "$CC9CXX" "$TMP/t_mimalloc.cpp"

cat > "$TMP/t_tommath.c" <<'EOF'
#include <tommath.h>
int f(mp_int* a) { return mp_init(a); }
EOF
smoke libtommath "$CC9CC" "$TMP/t_tommath.c"

# wuffs, exactly the way LibGfx/GIFLoader.cpp consumes it
cat > "$TMP/t_wuffs.cpp" <<'EOF'
#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__STATIC_FUNCTIONS
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE__CORE
#define WUFFS_CONFIG__MODULE__BASE__INTERFACES
#define WUFFS_CONFIG__MODULE__BASE__PIXCONV
#define WUFFS_CONFIG__MODULE__GIF
#define WUFFS_CONFIG__MODULE__LZW
#include <wuffs/wuffs-v0.3.c>
EOF
smoke wuffs "$CC9CXX" "$TMP/t_wuffs.cpp"

cat > "$TMP/t_fastfloat.cpp" <<'EOF'
#include <fast_float/fast_float.h>
double f(char const* b, char const* e) { double d = 0; fast_float::from_chars(b, e, d); return d; }
EOF
smoke fastfloat "$CC9CXX" "$TMP/t_fastfloat.cpp"

echo "== pkg-config (Ladybird consumes libtommath this way) =="
if PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" pkg-config --cflags --libs libtommath; then
	echo "ok   libtommath.pc"
else
	echo "FAIL libtommath.pc"; fail=1
fi

[ "$fail" = 0 ] && echo "ALL SMOKES PASS" || echo "SMOKE FAILURES"
exit "$fail"
