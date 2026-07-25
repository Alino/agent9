#!/bin/bash
# node9 on cc9 — build qjs with clang/LLVM instead of kencc.
#
# Why: the same benchmark run under both toolchains (node9/test/qjsbench) puts the
# interpreter 3.3x faster here, because kencc cannot do computed-goto dispatch
# (patch.sh has to set DIRECT_DISPATCH 0) and pcc's codegen is no match for
# clang -O2 on the bytecode loop. Startup and every JS-bound operation ride on that.
#
# What changes vs port/plan9:
#   - pristine quickjs sources: none of the kencc workarounds are needed (no NaN
#     compare guards, no alloca shim, no intptr_t rewrite, no arg-hoist mask)
#   - crypto/TLS/zlib come from ssl9 (OpenSSL) + zlib instead of Plan 9 libsec/libz,
#     which also means TLS certificates are actually verified
#   - real poll(2) and threads from cc9's runtime, no APE select shim
#
# Inputs (override with env):
#   QJS_SRC   pristine quickjs-ng tree (default /tmp/node9probe/src/quickjs-master)
#   CC9       cc9 checkout (default: sibling of this repo dir)
#   OSSL      OpenSSL for cc9 (default ../../ssl9)
#   ZLIB_A    libz.a built for cc9 (default ../../servo9/_out/libz.a)
# Output: _out/qjs-cc9 (a Plan 9 amd64 a.out)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
NODE9="$(cd "$HERE/../.." && pwd)"
QJS_SRC="${QJS_SRC:-/tmp/node9probe/src/quickjs-master}"
CC9="${CC9:-$ROOT/cc9}"
OSSL="${OSSL:-$ROOT/ssl9}"
ZLIB_A="${ZLIB_A:-$ROOT/servo9/_out/libz.a}"
ZLIB_INC="${ZLIB_INC:-$ROOT/servo9/port/mozjs/include}"
CLANG="${CLANG:-/opt/homebrew/opt/llvm/bin/clang}"
LLD="${LLD:-$(brew --prefix lld)/bin/ld.lld}"
OPT="${OPT:--O2}"

OUT="$HERE/_out"
mkdir -p "$OUT/obj"
cd "$OUT/obj"

[ -f "$QJS_SRC/quickjs.c" ] || { echo "no quickjs sources at $QJS_SRC (set QJS_SRC)"; exit 1; }
[ -f "$OSSL/_out/libssl.a" ] || { echo "no $OSSL/_out/libssl.a — build ssl9 first"; exit 1; }
[ -f "$ZLIB_A" ] || { echo "no $ZLIB_A — build zlib for cc9 first"; exit 1; }

# Engine sources: pristine except for the two upstream quickjs-libc event-loop bugs
# (they are engine bugs, not kencc workarounds, so port/plan9/patch.sh fixes the same two).
# Without them a streamed response sits unread until an unrelated fd goes ready — here it
# showed up as HTTP bodies truncating and pi turns stalling for tens of seconds.
SRC="$OUT/src"
rm -rf "$SRC"; mkdir -p "$SRC"
cp "$QJS_SRC"/*.c "$QJS_SRC"/*.h "$SRC/"
# js_os_poll_internal(): an expired timer returned before any fd was polled
perl -0777 -i -pe 's/        if \(min_delay == 0\)\n            return 0; \/\/ expired timer\n(        if \(min_delay < 0\))/$1/g' "$SRC/quickjs-libc.c"
# js_os_poll_internal(): nfds was overwritten with poll()'s ready COUNT, then used as the
# scan bound — poll() does not compact the array, so a ready fd at a later index is skipped
perl -0777 -i -pe 's/    nfds = poll\(pfds, nfds, min_delay\);\n    if \(nfds < 0\) \{/    if \(poll\(pfds, nfds, min_delay\) < 0\) \{/' "$SRC/quickjs-libc.c"
grep -q 'if (poll(pfds, nfds, min_delay) < 0)' "$SRC/quickjs-libc.c" || { echo "poll-dispatch fix did not apply"; exit 1; }
# libregexp.c: \p{...} is only parsed in /u mode, so with the ES2024 /v flag the
# escape falls through to the literal path and the PROPERTY NAME becomes a set of
# literal characters. /^[\p{Control}\p{Mark}...]+/v then matched 'a' (from
# "Default_Ignorable_Code_Point") and missed the code points it is supposed to
# match. pi-tui measures text with exactly that regex, so every styled line
# measured short, pi padded past the terminal width, its lines wrapped, and its
# cursor-up redraw landed a row off — duplicated lines with their tails missing.
perl -0777 -i -pe 's/(        case .p.:\n        case .P.:\n)            if \(s->is_unicode\) \{/$1            if \(s->is_unicode || s->unicode_sets\) \{/' "$SRC/libregexp.c"
# ...and /v IMPLIES /u. This engine keeps the two mutually exclusive ("invariant:
# is_unicode ^ unicode_sets"), so every unicode-semantics check was false under /v and
# the matcher walked UTF-16 code UNITS: \p{Surrogate} then matched half of an
# astral character. pi-tui strips leading non-printing characters with a class
# containing \p{Surrogate}, so an emoji measured 0 columns instead of 2 and every
# streaming redraw drifted two columns right. Per spec v does everything u does.
perl -0777 -i -pe 's/    s->is_unicode = \(\(re_flags & LRE_FLAG_UNICODE\) != 0\);/    s->is_unicode = ((re_flags & (LRE_FLAG_UNICODE | LRE_FLAG_UNICODE_SETS)) != 0);/' "$SRC/libregexp.c"
perl -0777 -i -pe 's/    s->is_unicode = \(re_flags & LRE_FLAG_UNICODE\) != 0;/    s->is_unicode = (re_flags & (LRE_FLAG_UNICODE | LRE_FLAG_UNICODE_SETS)) != 0;/' "$SRC/libregexp.c"
grep -c 'LRE_FLAG_UNICODE | LRE_FLAG_UNICODE_SETS' "$SRC/libregexp.c" | grep -qx 2 || { echo "v-implies-u fix did not apply"; exit 1; }
grep -q 'if (s->is_unicode || s->unicode_sets) {' "$SRC/libregexp.c" || { echo "unicode-property fix did not apply"; exit 1; }
# ...and /v also allows PROPERTIES OF STRINGS (\p{RGI_Emoji} and friends), which this
# engine has no tables for. Parsing them as an error would now break module loading
# for code that only feature-tests them (pi-tui builds one at import time), so in /v
# they resolve to the empty set: they match nothing, which is what the old
# fall-through did anyway, while real character properties are now correct.
python3 - "$SRC/libregexp.c" <<'PYEOF_INNER'
import sys
p = sys.argv[1]; s = open(p).read()
old = """    } else {
    unknown_property_name:
        return re_parse_error(s, "unknown unicode property name");
    }"""
new = """    } else {
    unknown_property_name:
        /* properties of strings (\\p{RGI_Emoji} and friends) are /v-only and this
           engine has no tables for them. Erroring would break module loading for
           code that merely feature-tests one (pi-tui builds such a regex at import
           time), so they resolve to the empty set: they match nothing, exactly as
           the old literal fall-through did, while real character properties work. */
        if (s->unicode_sets && (!strcmp(name, "RGI_Emoji") ||
                                !strcmp(name, "Basic_Emoji") ||
                                !strcmp(name, "Emoji_Keycap_Sequence") ||
                                !strcmp(name, "RGI_Emoji_Modifier_Sequence") ||
                                !strcmp(name, "RGI_Emoji_Flag_Sequence") ||
                                !strcmp(name, "RGI_Emoji_Tag_Sequence") ||
                                !strcmp(name, "RGI_Emoji_ZWJ_Sequence"))) {
            cr_init(cr, s->opaque, lre_realloc);
        } else {
            return re_parse_error(s, "unknown unicode property name");
        }
    }"""
assert old in s, "unknown_property_name block not found"
open(p, "w").write(s.replace(old, new, 1))
PYEOF_INNER
grep -q 'properties of strings' "$SRC/libregexp.c" || { echo "string-property fallback did not apply"; exit 1; }
grep -c 'expired timer' "$SRC/quickjs-libc.c" | grep -qx 1 || { echo "expired-timer fix did not apply"; exit 1; }
QJS_SRC="$SRC"

# Only the two portability defines the runtime needs.
CFLAGS=(--target=x86_64-unknown-none -nostdlib -DNDEBUG -DN9_CC9 $OPT
        -DNO_TM_GMTOFF -Dalloca=__builtin_alloca
        -include "$HERE/n9_cc9_compat.h"
        -isystem "$CC9/runtime/include"
        -fno-pic -femulated-tls -funwind-tables)

echo "== engine (pristine, DIRECT_DISPATCH on)"
for f in quickjs libregexp libunicode dtoa quickjs-libc; do
  "$CLANG" "${CFLAGS[@]}" -I"$QJS_SRC" -c "$QJS_SRC/$f.c" -o "$f.o"
done

echo "== node9 glue"
"$CLANG" "${CFLAGS[@]}" -I"$QJS_SRC" -I"$NODE9/port/plan9" -I"$HERE" \
  -c "$NODE9/port/plan9/n9_cli.c" -o n9_cli.o
"$CLANG" "${CFLAGS[@]}" -DN9_TLS_HANDLES -I"$QJS_SRC" -I"$NODE9/port/plan9" -I"$HERE" \
  -c "$NODE9/port/plan9/n9_native.c" -o n9_native.o
"$CLANG" "${CFLAGS[@]}" -I"$NODE9/port/plan9" -I"$HERE" \
  -I"$OSSL/vendor/openssl-3.0.17/include" -I"$ZLIB_INC" \
  -c "$HERE/n9_sec_ossl.c" -o n9_sec_ossl.o

echo "== link"
"$LLD" -o "$OUT/qjs-cc9.elf" \
  n9_cli.o n9_native.o n9_sec_ossl.o quickjs.o libregexp.o libunicode.o dtoa.o quickjs-libc.o \
  "$OSSL/_out/libssl.a" "$OSSL/_out/libcrypto.a" "$ZLIB_A" \
  --start-group "$CC9/lib/libcc9cxx.a" "$CC9/lib/libcc9m.a" --end-group \
  -T "$CC9/test/plan9.ld" -static -nostdlib

python3 "$CC9/host/elf2aout.py" "$OUT/qjs-cc9.elf" "$OUT/qjs-cc9"
ls -l "$OUT/qjs-cc9"
