#!/bin/sh
# make-tarball.sh — assemble python9-amd64.tar.gz for pac9's `tarball` install.
#
# The cc9 build of CPython 3.11.14 for 9front: a general-purpose Python with a
# batteries-included set of C/Rust extensions STATICALLY linked in (Plan 9 has
# no dynamic loading, so everything lives in one a.out):
#   _ssl/_hashlib (OpenSSL 3.0.17), zlib, _sqlite3, _blake2/_sha3,
#   _pydantic_core + jiter (Rust/PyO3, static-embedded).
# Real pthreads, poll(2), sockets over /net.  Supersedes the old kencc `py`.
#
# Layout at / :
#   /usr/glenda/python9/bin/python3     the interpreter a.out
#   /sys/lib/python/lib/python3.11/     the stdlib (compiled-in PREFIX)
#   /rc/bin/{python,python3}            wrappers
#
# Publish:  gh release create python9 python9-amd64.tar.gz -t 'python9 — Python 3.11 for 9front (cc9)'
set -e
HERE=$(cd "$(dirname "$0")" && pwd); PY9=$(dirname "$HERE"); AGENT9=$(dirname "$PY9")
BIN="${PY9_BIN:-$PY9/port/cc9/_out/python.aout}"
SRC="$PY9/cpython/src"
SYSCONF="$PY9/port/plan9/_sysconfigdata__plan9_.py"
STAGE="$HERE/stage"; TARBALL="$HERE/python9-amd64.tar.gz"

[ -f "$BIN" ] || { echo "missing interpreter $BIN — run port/cc9/build.py first"; exit 1; }
[ -d "$SRC/Lib" ] || { echo "missing stdlib $SRC/Lib"; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE/usr/glenda/python9/bin" "$STAGE/sys/lib/python/lib/python3.11" "$STAGE/rc/bin"

cp "$BIN" "$STAGE/usr/glenda/python9/bin/python3"
chmod +x "$STAGE/usr/glenda/python9/bin/python3"

# stdlib (skip the heavy dev/test trees not needed at runtime)
( cd "$SRC" && COPYFILE_DISABLE=1 tar --format ustar -cf - \
    --exclude 'test' --exclude '*/test' --exclude 'tests' --exclude 'idlelib' \
    --exclude 'tkinter' --exclude 'turtledemo' --exclude '__pycache__' --exclude '*.pyc' \
    Lib ) | ( cd "$STAGE/sys/lib/python/lib/python3.11" && tar xf - )
mv "$STAGE/sys/lib/python/lib/python3.11/Lib/"* "$STAGE/sys/lib/python/lib/python3.11/"
rm -rf "$STAGE/sys/lib/python/lib/python3.11/Lib"
cp "$SYSCONF" "$STAGE/sys/lib/python/lib/python3.11/_sysconfigdata__plan9_.py"
# empty lib-dynload/ silences CPython getpath's "Could not find platform
# dependent libraries" warning (static build has no dynload modules).
mkdir -p "$STAGE/sys/lib/python/lib/python3.11/lib-dynload"
: > "$STAGE/sys/lib/python/lib/python3.11/lib-dynload/.keep"

for w in python python3; do
cat > "$STAGE/rc/bin/$w" <<'EOF'
#!/bin/rc
exec /usr/glenda/python9/bin/python3 $*
EOF
chmod +x "$STAGE/rc/bin/$w"
done

( cd "$STAGE" && COPYFILE_DISABLE=1 tar --format ustar -czf "$TARBALL" usr sys rc )
echo "-> $TARBALL  ($(du -h "$TARBALL" | cut -f1))"
