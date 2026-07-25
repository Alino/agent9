#!/bin/sh
# make-tarball.sh — assemble node9-amd64.tar.gz for pac9's `tarball` install.
#
# Layout at / :
#   /amd64/bin/qjs                  the runtime (cc9/clang build, static a.out)
#   /amd64/bin/{node,npm}           rc wrappers
#   /amd64/lib/node9/boot.js        the Node-compatible standard library
#   /amd64/lib/node9/ca.pem         CA bundle — TLS certificates are verified
#   /amd64/lib/node9/npm/           npm 10.9.8 (patched), unchanged since 0.1.0
#   /sys/lib/pac9/changelog/node9
#
# The engine is the cc9 build (port/cc9/build.sh), which cross-builds on the host;
# the older kencc build is compiled on the box and is not packaged.
set -e
HERE=$(cd "$(dirname "$0")" && pwd); N9=$(dirname "$HERE"); AGENT9=$(dirname "$N9")

QJS="${NODE9_QJS:-$N9/port/cc9/_out/qjs-cc9}"
BOOT="$N9/lib/boot.js"
CA="${NODE9_CA:-$AGENT9/ladybird9/port/assets/certs/ca.pem}"
# npm has not changed since 0.1.0 and is not vendored in this repo (patch.sh
# fetches and patches it), so take the shipped tree from that release.
NPM_SRC="${NODE9_NPM:-$HERE/.cache/npm}"
PREV=https://github.com/Alino/agent9/releases/download/node9-v0.1.0/node9-amd64.tar.gz

[ -f "$QJS" ] || { echo "missing engine $QJS — run port/cc9/build.sh first"; exit 1; }
[ -f "$BOOT" ] || { echo "missing $BOOT"; exit 1; }
[ -f "$CA" ] || { echo "missing CA bundle $CA (set NODE9_CA)"; exit 1; }

if [ ! -d "$NPM_SRC" ]; then
  echo "== fetching the npm tree from node9-v0.1.0"
  mkdir -p "$HERE/.cache"
  curl -sL "$PREV" -o "$HERE/.cache/prev.tar.gz"
  tar xzf "$HERE/.cache/prev.tar.gz" -C "$HERE/.cache" amd64/lib/node9/npm
  mv "$HERE/.cache/amd64/lib/node9/npm" "$HERE/.cache/npm"
  rm -rf "$HERE/.cache/amd64" "$HERE/.cache/prev.tar.gz"
fi

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/amd64/bin" "$stage/amd64/lib/node9" "$stage/sys/lib/pac9/changelog"

cp "$QJS" "$stage/amd64/bin/qjs"; chmod +x "$stage/amd64/bin/qjs"
cp "$BOOT" "$stage/amd64/lib/node9/boot.js"
cp "$CA" "$stage/amd64/lib/node9/ca.pem"
cp -R "$NPM_SRC" "$stage/amd64/lib/node9/npm"
cp "$HERE/CHANGELOG" "$stage/sys/lib/pac9/changelog/node9"

cat > "$stage/amd64/bin/node" <<'EOF'
#!/bin/rc
exec /amd64/bin/qjs $*
EOF
cat > "$stage/amd64/bin/npm" <<'EOF'
#!/bin/rc
exec /amd64/bin/qjs /amd64/lib/node9/npm/bin/npm-cli.js $*
EOF
chmod +x "$stage/amd64/bin/node" "$stage/amd64/bin/npm"

# ustar so 9front's tar reads it; no macOS xattr turds.
COPYFILE_DISABLE=1 tar --format ustar -C "$stage" -czf "$HERE/node9-amd64.tar.gz" amd64 sys
ls -l "$HERE/node9-amd64.tar.gz"
