#!/bin/sh
# release.sh <pkg> <version> — cut an immutable versioned pac9 release.
#
# The one command that keeps GitHub Releases and the registry from drifting:
#   1. creates the GitHub release  <pkg>-v<version>  with the prebuilt tarball
#      and the package's CHANGELOG as the release notes (--latest=false, so no
#      component release grabs the repo's "Latest" badge — monorepo hygiene);
#   2. repoints pac9/registry's <pkg> row at the immutable URL and sets its
#      version column, PRESERVING the deps column.
# You still: build the tarball first (<pkg>/release/make-tarball.sh) and, after
# this, `git add pac9/registry && git commit && git push` so `pac9 update` serves
# the new URL.
#
# Immutable by construction: `gh release create` fails if <pkg>-v<version>
# already exists — a version never moves. Bump the version instead.
#
# Env: GH_TOKEN must be set (e.g. `export GH_TOKEN=$GITHUB_PAT`).
set -e
pkg=$1; ver=$2
[ -n "$pkg" ] && [ -n "$ver" ] || { echo "usage: release.sh <pkg> <version>"; exit 1; }

root=$(cd "$(dirname "$0")/.." && pwd)
repo=Alino/agent9
tb="$root/$pkg/release/$pkg-amd64.tar.gz"
cl="$root/$pkg/release/CHANGELOG"
reg="$root/pac9/registry"
tag="$pkg-v$ver"

[ -f "$tb" ] || { echo "no tarball: $tb  (run $pkg/release/make-tarball.sh first)"; exit 1; }
[ -f "$cl" ] || { echo "no CHANGELOG: $cl"; exit 1; }
grep -q "^$pkg	" "$reg" || { echo "no registry entry for '$pkg' in $reg"; exit 1; }

# 1. the immutable release (create fails loudly if the version already exists)
gh release create "$tag" "$tb" --repo "$repo" \
  -t "$pkg $ver — 9front" -F "$cl" --latest=false

# 2. repoint the registry row, preserving its deps (5th column)
url="https://github.com/$repo/releases/download/$tag/$pkg-amd64.tar.gz"
awk -F'\t' -v p="$pkg" -v url="$url" -v ver="$ver" '
  $1==p { d=(NF>=5 && $5!="")?$5:"-"; print p"\t-\t.\ttarball "url"\t"d"\t"ver; next }
  { print }
' "$reg" > "$reg.tmp" && mv "$reg.tmp" "$reg"

echo "released $tag + repointed registry."
echo "next:  git -C $root add pac9/registry && git commit -m 'pac9: $pkg $ver' && git push"
