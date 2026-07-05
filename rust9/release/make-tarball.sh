#!/bin/sh
# make-tarball.sh — assemble rust9-amd64.tar.gz for pac9's `tarball` install:
# the self-hosted Rust toolchain, laid out at / . Contents:
#   /usr/glenda/rust/bin/{rustc,n9link,cargo9}   the compiler, linker, mini-cargo
#   /usr/glenda/rust/lib/rustlib/x86_64-unknown-plan9/lib/   the std sysroot
#   /usr/glenda/rust/cc9lib/{n9unwind.o,libcc9cxx.a,libcc9m.a}   n9link substrate
#   /rc/bin/{rustc,cargo9}   wrappers (absolute argv0 -> sysroot; -C linker=n9link)
# The prefix is /usr/glenda/rust because rustc derives its sysroot from
# argv[0]/../.. and n9link's compiled-in $N9LINK_LIB default is
# /usr/glenda/rust/cc9lib — the layout proven live on cirno.
#
# Prereqs (see rust9/RUSTC-PORT.md):
#   - rustc a.out:  x.py build --stage 2 --host x86_64-unknown-plan9 compiler/rustc
#   - sysroot:      x.py build --stage 1 library --target x86_64-unknown-plan9
#   - n9link/cargo9: prebuilt/ (fetched from the box; sources cc9/host/n9link.c)
#
# Publish as the GitHub release `rust9` asset:
#   gh release create rust9 rust9-amd64.tar.gz -t 'rust9 — Rust for 9front'
set -e
HERE=$(cd "$(dirname "$0")" && pwd); RUST9=$(dirname "$HERE"); AGENT9=$(dirname "$RUST9")
RSRC="${RUST_SRC_FULL:-$HOME/Projects/rust-src-full}"
RUSTC_AOUT="${RUSTC_AOUT:-$RSRC/build/aarch64-apple-darwin/stage2-rustc/x86_64-unknown-plan9/release/rustc-main}"
SYSROOT_LIB="${SYSROOT_LIB:-$RSRC/build/host/stage1/lib/rustlib/x86_64-unknown-plan9/lib}"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
STAGE="$HERE/stage"; TARBALL="$HERE/rust9-amd64.tar.gz"

[ -f "$RUSTC_AOUT" ] || { echo "missing $RUSTC_AOUT — build rustc first (see header)"; exit 1; }
[ -d "$SYSROOT_LIB" ] || { echo "missing $SYSROOT_LIB — build the stage1 library first"; exit 1; }

R="$STAGE/usr/glenda/rust"
rm -rf "$STAGE"
mkdir -p "$R/bin" "$R/cc9lib" "$R/lib/rustlib/x86_64-unknown-plan9" "$STAGE/rc/bin"

cp "$RUSTC_AOUT" "$R/bin/rustc"
cp "$HERE/prebuilt/n9link" "$HERE/prebuilt/cargo9" "$R/bin/"
chmod +x "$R/bin/"*

cp "$AGENT9/cc9/lib/libcc9cxx.a" "$AGENT9/cc9/lib/libcc9m.a" "$R/cc9lib/"
# same flags rust9-ld uses for the unwind shim
"$LLVM/clang" --target=x86_64-unknown-none -ffreestanding -nostdlib -fno-pic -O2 \
	-c "$RUST9/host/n9unwind.c" -o "$R/cc9lib/n9unwind.o"

cp -R "$SYSROOT_LIB" "$R/lib/rustlib/x86_64-unknown-plan9/lib"
rm -rf "$R/lib/rustlib/x86_64-unknown-plan9/lib/self-contained"

cat > "$STAGE/rc/bin/rustc" <<'EOF'
#!/bin/rc
# rust9: the real rustc (cranelift backend), self-hosted on 9front.
# Absolute argv0 matters: rustc derives its sysroot from argv[0]/../..
# NB rc treats bare = as syntax: quote '--emit=metadata' or use '--emit metadata'.
exec /usr/glenda/rust/bin/rustc '-C' 'linker=/usr/glenda/rust/bin/n9link' $*
EOF
cat > "$STAGE/rc/bin/cargo9" <<'EOF'
#!/bin/rc
# rust9: minimal cargo (build/run/clean local-path workspaces) driving rustc+n9link.
exec /usr/glenda/rust/bin/cargo9 $*
EOF
chmod +x "$STAGE/rc/bin/rustc" "$STAGE/rc/bin/cargo9"

# ustar format so 9front's tar reads it; COPYFILE_DISABLE stops macOS ._* junk.
( cd "$STAGE" && COPYFILE_DISABLE=1 tar --format ustar -czf "$TARBALL" usr rc )
echo "-> $TARBALL  ($(du -h "$TARBALL" | cut -f1))"
