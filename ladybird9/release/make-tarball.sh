#!/bin/sh
# make-tarball.sh — assemble ladybird9-amd64.tar.gz for pac9's `tarball` install:
# the cross-compiled Ladybird browser, its 5 helper processes, resources, ICU
# data, the gl9win2 presenter, and an rc launcher — laid out at / . Contents:
#   /usr/glenda/ladybird9/bin/ladybird              main UI (resource-root anchor)
#   /usr/glenda/ladybird9/libexec/{WebContent,Compositor,RequestServer,
#                                  ImageDecoder,WebWorker}   helper processes
#   /usr/glenda/ladybird9/share/Lagom/...           themes, fonts, about pages
#   /usr/glenda/ladybird9/share/icu/icudt78l.dat    ICU data (ICU_DATA env)
#   /usr/glenda/ladybird9/bin/gl9win2               libdraw window presenter (native;
#                                                   package-PRIVATE: alacritty9/dosbox9
#                                                   also ship /amd64/bin/gl9win2, and
#                                                   pac9 uninstall of one would delete
#                                                   a shared copy under the other)
#   /rc/bin/ladybird                                launcher: gl9win2 + browser
#   /sys/lib/pac9/changelog/ladybird9               pac9 changelog
#
# The prefix is /usr/glenda/ladybird9 because the browser derives its resource
# root from argv[0] (find_prefix -> <prefix>/share/Lagom, LibWebView/Utilities.cpp).
# Ladybird can't build on 9front (needs host clang/cmake/rust), so this ships the
# Mac-cross-compiled a.out artifacts, exactly like rust9.
#
# Prereqs:
#   - the ladybird9 build: ladybird9/host/build-ladybird.sh (bin/ladybird +
#     libexec/* + share/Lagom), CC9_LIBCC9CXX=<known-good runtime archive>
#   - ICU data: ladybird9/_out/deps/share/icu/78.3/icudt78l.dat
#   - gl9win2 (with the GL9B op): release/prebuilt/gl9win2, built on a 9front box
#     via alacritty9/win/mkfile (objtype=amd64; mk)
#
# Publish as a VERSIONED GitHub release (pac9 doctrine: a registry version
# column requires an immutable URL, so the tag embeds the version):
#   V=0.1.0
#   gh release create ladybird9-v$V ladybird9-amd64.tar.gz -t "ladybird9 v$V — Ladybird for 9front"
set -e
HERE=$(cd "$(dirname "$0")" && pwd); LB9=$(dirname "$HERE"); AGENT9=$(dirname "$LB9")
BUILD="${LB9_BUILD:-$LB9/_out/build}"
ICU_DAT="${ICU_DAT:-$LB9/_out/deps/share/icu/78.3/icudt78l.dat}"
GL9WIN2="${GL9WIN2:-$HERE/prebuilt/gl9win2}"
STAGE="$HERE/stage"; TARBALL="$HERE/ladybird9-amd64.tar.gz"

[ -f "$BUILD/bin/ladybird" ] || { echo "missing $BUILD/bin/ladybird — build ladybird9 first"; exit 1; }
[ -f "$ICU_DAT" ]           || { echo "missing $ICU_DAT — build the ICU dep first"; exit 1; }
[ -f "$GL9WIN2" ]           || { echo "missing $GL9WIN2 — build gl9win2 (GL9B) on a 9front box"; exit 1; }
[ -d "$BUILD/share/Lagom" ] || { echo "missing $BUILD/share/Lagom resources"; exit 1; }

P="$STAGE/usr/glenda/ladybird9"
rm -rf "$STAGE"
mkdir -p "$P/bin" "$P/libexec" "$P/share/icu" "$STAGE/rc/bin" \
         "$STAGE/sys/lib/pac9/changelog"

# cc9-link already emits the final a.out (file == "data"); ship as-is (no elf2aout).
cp "$BUILD/bin/ladybird" "$P/bin/ladybird"
for h in WebContent Compositor RequestServer ImageDecoder WebWorker; do
	cp "$BUILD/libexec/$h" "$P/libexec/$h"
done
chmod +x "$P/bin/ladybird" "$P/libexec/"*

cp -R "$BUILD/share/Lagom" "$P/share/Lagom"
cp "$ICU_DAT" "$P/share/icu/icudt78l.dat"

# CA bundle (Mozilla roots via curl.se): stock 9front has no public CA bundle at
# curl's compiled-in /sys/lib/tls/ca.pem, so HTTPS failed until the user hand-
# fetched one (issue #23). Bundle it and point RequestServer at it via the
# launcher's --certificate, so HTTPS works out of the box.
cp "$HERE/../port/assets/certs/ca.pem" "$P/share/ca.pem"
cp "$GL9WIN2" "$P/bin/gl9win2"
chmod +x "$P/bin/gl9win2"

# Launcher: run the browser interactively under the gl9win2 presenter (the
# package-private copy — see the header note about the /amd64/bin collision).
# LibDatabase now opens SQLite with the "unix-none" VFS on Plan 9 (no fcntl
# locks) so a persistent profile CAN open — but reopening it on the next launch
# currently wedges (a teardown-leftover holds the store), so we keep
# --temporary-profile for reliable startup until that's hardened. SQL itself
# works, so --disable-sql-database is dropped (cookies/storage work in-session).
cat > "$STAGE/rc/bin/ladybird" <<'EOF'
#!/bin/rc
# ladybird9: the real Ladybird browser on 9front (interactive, via gl9win2).
#   ladybird https://example.com     open a page
#   ladybird --headless --screenshot-path shot.png <url>   (run the binary directly)
ICU_DATA=/usr/glenda/ladybird9/share/icu
exec /usr/glenda/ladybird9/bin/gl9win2 /usr/glenda/ladybird9/bin/ladybird '--temporary-profile' '--certificate' /usr/glenda/ladybird9/share/ca.pem $*
EOF
chmod +x "$STAGE/rc/bin/ladybird"

cp "$HERE/CHANGELOG" "$STAGE/sys/lib/pac9/changelog/ladybird9"

# ustar so 9front's tar reads it; COPYFILE_DISABLE stops macOS ._* junk.
( cd "$STAGE" && COPYFILE_DISABLE=1 tar --format ustar -czf "$TARBALL" usr rc sys )
echo "-> $TARBALL  ($(du -h "$TARBALL" | cut -f1))"
echo "registry entry (tab-separated: name url subdir recipe deps version):"
echo "ladybird9	-	.	tarball https://github.com/Alino/agent9/releases/download/ladybird9-v0.2.0/ladybird9-amd64.tar.gz	-	0.2.0"
