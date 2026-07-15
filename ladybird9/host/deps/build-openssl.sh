#!/bin/bash
# build-openssl.sh — cross-build OpenSSL 3.5.7 for 9front via cc9, publish into
# the ladybird9 sysroot (_out/deps).
#
# Heritage: this replaces the old copy-from-ssl9 recipe (OpenSSL 3.0.17, built
# by ssl9/build.py). Ladybird's LibCrypto needs 3.2+/3.5 APIs — <openssl/thread.h>
# (OSSL_set_max_threads for Argon2), OSSL_SIGNATURE_PARAM_CONTEXT_STRING, and
# OSSL_PKEY_PARAM_ML_DSA_SEED (ML-DSA post-quantum) — so ladybird9 now owns its
# own OpenSSL build. The harvest pattern mirrors ssl9/build.py 1:1: perl
# Configure on the HOST generates config headers + the source graph, then every
# .c is recompiled with cc9's clang->Plan9 flags and archived with llvm-ar.
#
# ssl9-proven traps carried over:
#   - do NOT pass no-sock  (compiles out SSL_set_fd / socket BIO; TLS runs over net9)
#   - do NOT pass no-deprecated  (legacy RSA/SRP TUs + consumers need it)
#   - -DOPENSSL_NO_SECURE_MEMORY  (Plan 9 / cc9 has no mmap/mlock)
#   - -femulated-tls, --with-rand-seed=devrandom  (cc9 maps /dev/urandom)
# 3.5 notes:
#   - threads + thread-pool + default-thread-pool are Configure defaults; left ON
#     (cc9 has real pthreads), so OSSL threads / Argon2 multi-lane work.
#   - `make build_all_generated` now materializes everything (der gens,
#     params_idx.c, buildinf.h) — no hand-run dofile.pl step like 3.0 needed.
#   - libcrypto member list comes from configdata's unified_info (libcrypto +
#     providers/libcommon.a fold-in), not a directory glob; the cross-boundary
#     file is ssl/record/methods/ssl3_cbc.c and the walk picks it up itself.
. "$(dirname "$0")/common.sh"

URL="https://github.com/openssl/openssl/releases/download/openssl-3.5.7/openssl-3.5.7.tar.gz"
SHA="a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8"
SRC="$VENDOR/openssl-3.5"
CC9="$AGENT9/cc9"

fetch "$URL" "$SHA" "$SRC"

# --- Plan 9 compile shims (same two files ssl9/port/include proved out) -----
SHIM="$SRC/.port-include"
mkdir -p "$SHIM"
echo '#include <string.h>' > "$SHIM/memory.h"
cat > "$SHIM/syslog.h" <<'EOF'
#ifndef _SYSLOG_H
#define _SYSLOG_H
/* Plan 9 has no syslog; OpenSSL's bss_log.c only needs the constants + a
 * no-op openlog/syslog/closelog to compile. Logs go nowhere. */
#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_NOTICE 5
#define LOG_INFO 6
#define LOG_DEBUG 7
#define LOG_PID 0x01
#define LOG_CONS 0x02
#define LOG_DAEMON (3<<3)
#define LOG_USER (1<<3)
#define LOG_LOCAL0 (16<<3)
#define LOG_LOCAL1 (17<<3)
#define LOG_LOCAL2 (18<<3)
#define LOG_LOCAL3 (19<<3)
#define LOG_LOCAL4 (20<<3)
#define LOG_LOCAL5 (21<<3)
#define LOG_LOCAL6 (22<<3)
#define LOG_LOCAL7 (23<<3)
static void openlog(const char *a, int b, int c) { (void)a;(void)b;(void)c; }
static void syslog(int a, const char *b, ...) { (void)a;(void)b; }
static void closelog(void) {}
#endif
EOF

# --- host Configure + generated sources (stamp-guarded, idempotent) ---------
if [ ! -f "$SRC/configdata.pm" ]; then
	(cd "$SRC" && perl ./Configure linux-x86_64 \
		no-asm no-shared no-dso no-engine no-tests no-ui-console \
		threads --with-rand-seed=devrandom)
fi
if [ ! -f "$SRC/crypto/buildinf.h" ]; then
	(cd "$SRC" && make build_all_generated -j"$(sysctl -n hw.ncpu)" >/dev/null)
fi

# --- source lists straight from the build graph -----------------------------
(cd "$SRC" && perl -I. -Mconfigdata -e '
	sub leafsrcs { my ($t,$seen)=@_; my @out;
		for my $x (@{$unified_info{sources}{$t}//[]}) {
			next if $seen->{$x}++;
			if ($x =~ /\.c$/) { push @out, $x } else { push @out, leafsrcs($x,$seen) }
		}
		@out;
	}
	my %seen;
	open my $c, ">", ".srcs-crypto.txt"; print $c "$_\n"
		for sort(leafsrcs("libcrypto",\%seen), leafsrcs("providers/libcommon.a",\%seen));
	open my $s, ">", ".srcs-ssl.txt"; print $s "$_\n" for sort(leafsrcs("libssl",{}));
')

# --- harvest compile: every TU with cc9 clang flags, llvm-ar archives -------
# (flags verbatim from ssl9/build.py; only the version + include set changed)
SRC="$SRC" CC9="$CC9" LLVM="$LLVM" SHIM="$SHIM" python3 - <<'PY'
import os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

SRC, CC9, LLVM, SHIM = (os.environ[k] for k in ("SRC","CC9","LLVM","SHIM"))
OBJ = os.path.join(SRC, ".obj9")

CFLAGS = [
    "--target=x86_64-unknown-none", "-nostdlib", "-O2",
    "-femulated-tls", "-fno-pic", "-mno-red-zone", "-funwind-tables",
    "-D__plan9__", "-DNDEBUG", "-DOPENSSL_NO_ASM", "-DSTATIC_LEGACY",
    "-DOPENSSL_NO_SECURE_MEMORY",     # no mmap/mlock on Plan 9
    '-DOPENSSLDIR="/sys/lib/ssl"',
    '-DENGINESDIR="/sys/lib/ssl/engines"',
    '-DMODULESDIR="/sys/lib/ssl/modules"',
    "-Wno-macro-redefined", "-Wno-implicit-function-declaration",
    "-isystem", os.path.join(CC9, "runtime", "include"),
    "-isystem", SHIM,
]
for i in (".", "include", "crypto", "providers/common/include",
          "providers/implementations/include", "providers/fips/include"):
    CFLAGS += ["-I", os.path.join(SRC, i)]

def load(name):
    return [l.strip() for l in open(os.path.join(SRC, name)) if l.strip()]

def obj_path(rel):
    return os.path.join(OBJ, re.sub(r"[/.]", "__", rel) + ".o")

def compile_one(rel):
    src, obj = os.path.join(SRC, rel), obj_path(rel)
    if os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(src):
        return None
    p = subprocess.run([os.path.join(LLVM, "clang"), *CFLAGS, "-c", src, "-o", obj],
                       capture_output=True, text=True)
    return None if p.returncode == 0 else (rel, p.stderr)

os.makedirs(OBJ, exist_ok=True)
crypto, ssl = load(".srcs-crypto.txt"), load(".srcs-ssl.txt")
print("libcrypto %d TUs, libssl %d TUs" % (len(crypto), len(ssl)))
fails = []
with ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
    fails = [r for r in ex.map(compile_one, crypto + ssl) if r]
if fails:
    buckets = {}
    for rel, err in fails:
        m = re.search(r"error: (.+)", err)
        buckets.setdefault(m.group(1)[:70] if m else "??", []).append(rel)
    print("\n%d TUs failed, grouped by first error:" % len(fails))
    for key, rels in sorted(buckets.items(), key=lambda kv: -len(kv[1])):
        print("  [%3d] %s" % (len(rels), key))
        for r in rels[:3]:
            print("         e.g. %s" % r)
    sys.exit(1)
for lib, srcs in (("libcrypto", crypto), ("libssl", ssl)):
    ar = os.path.join(OBJ, lib + ".a")
    if os.path.exists(ar): os.remove(ar)
    subprocess.check_call([os.path.join(LLVM, "llvm-ar"), "rcs", ar,
                           *[obj_path(s) for s in srcs]])
    print("-> %s (%d objs)" % (ar, len(srcs)))
PY

# --- install into the sysroot (wipe 3.0.17 headers so none linger) ----------
rm -rf "$PREFIX/include/openssl"
mkdir -p "$PREFIX/include/openssl"
cp "$SRC"/include/openssl/*.h "$PREFIX/include/openssl/"   # generated .h are in-tree
cp "$SRC/.obj9/libssl.a" "$SRC/.obj9/libcrypto.a" "$PREFIX/lib/"
echo "openssl 3.5.7 published to $PREFIX"

# --- acceptance 1+2: arch + 3.5-API smoke TU compiled AND linked via cc9 ----
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
for a in libssl libcrypto; do
	bad=$("$LLVM/llvm-objdump" -f "$PREFIX/lib/$a.a" 2>&1 | grep 'file format' | grep -cv elf64-x86-64 || true)
	[ "$bad" -eq 0 ] || { echo "FAIL: $a.a has $bad non-elf64-x86-64 members" >&2; exit 1; }
done
cat > "$TMP/smoke.c" <<'EOF'
#include <openssl/evp.h>
#include <openssl/thread.h>       /* 3.2+: OSSL thread pool API */
#include <openssl/core_names.h>
int main(void) {
    OSSL_set_max_threads(NULL, 4);                          /* Argon2 lanes */
    const char *ml  = OSSL_PKEY_PARAM_ML_DSA_SEED;          /* 3.5 ML-DSA */
    const char *ctx = OSSL_SIGNATURE_PARAM_CONTEXT_STRING;  /* 3.2+ */
    EVP_MD *md = EVP_MD_fetch(NULL, "SHA2-256", NULL);
    unsigned char d[64]; unsigned int n;
    EVP_Digest(ml, 1, d, &n, EVP_sha256(), NULL);
    (void)ctx; EVP_MD_free(md);
    return md ? 0 : 1;
}
EOF
"$CC9CC" "$TMP/smoke.c" -I"$PREFIX/include" \
	"$PREFIX/lib/libssl.a" "$PREFIX/lib/libcrypto.a" -o "$TMP/smoke.elf"
echo "smoke: EVP_MD_fetch + ML_DSA_SEED + <openssl/thread.h> compile+link OK"

# --- acceptance 3: libcurl.a's undefined syms all resolve post-upgrade ------
if [ -f "$PREFIX/lib/libcurl.a" ]; then
	NM="$LLVM/llvm-nm"
	"$NM" -u "$PREFIX/lib/libcurl.a" 2>/dev/null | awk '$1=="U"{print $2}' | sort -u > "$TMP/curl.undef"
	"$NM" --defined-only "$PREFIX"/lib/*.a "$CC9/lib/libcc9cxx.a" "$CC9/lib/libcc9m.a" 2>/dev/null \
		| awk 'NF>=3{print $3}' | sort -u > "$TMP/all.def"
	missing=$(comm -23 "$TMP/curl.undef" "$TMP/all.def")
	if [ -n "$missing" ]; then
		echo "FAIL: libcurl.a undefined symbols unresolved after 3.5 upgrade:" >&2
		echo "$missing" | sed 's/^/  /' >&2
		exit 1
	fi
	echo "curl audit: all $(wc -l < "$TMP/curl.undef" | tr -d ' ') undefined syms resolve"
fi
