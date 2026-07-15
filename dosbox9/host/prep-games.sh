#!/bin/bash
# prep-games.sh — extract the fetched game zips into a flat DOS C: tree and
# tar it for shipping to the 9front box.
#
# Layout: c/<slug>/<game files>   (slugs are all <=8 chars: DOS 8.3)
# Output: <dir>/c.tar
#
# Two shapes of zip:
#   ready   — the game files are in the zip (possibly under a subdir)
#   payload — the zip holds an installer + a compressed blob (Apogee/id shipped
#             DEICE/INSTALL floppies). Those blobs are ZIPs behind a small
#             header, so 7z extracts them directly and we never have to run a
#             DOS installer.
set -euo pipefail
G="${1:-/tmp/dosgames}"
C="$G/c"
rm -rf "$C"; mkdir -p "$C"

command -v 7z >/dev/null || { echo "need 7z (brew install p7zip)"; exit 1; }

# slug:subdir:payload-blobs
#   subdir  "." = zip root; else the dir inside the zip holding the game
#   payload "-" = none; else space-free list separated by ','
GAMES="
keen1:.:-
jill:JillJung:-
raptor:raptor:-
blake:.:-
duke2:.:-
keen4:CKeen4:-
bmenace:.:-
caves:.:-
harry:Halloween_Harry:-
hocus:HocusPoc/HOCUS:-
wolf3d:.:W3D1_BBS._1
doom:.:DOOM18S.1,DOOM18S.2
"

for ent in $GAMES; do
  slug="${ent%%:*}"; rest="${ent#*:}"
  sub="${rest%%:*}"; payload="${rest#*:}"
  z="$G/$slug.zip"
  [ -f "$z" ] || { echo "skip $slug (no zip)"; continue; }
  tmp=$(mktemp -d)
  unzip -qq -o "$z" -d "$tmp" 2>/dev/null || true

  if [ "$payload" != "-" ]; then
    # installer floppy: crack the blob(s) open instead of running the installer
    mkdir -p "$tmp/_out"
    IFS=',' read -ra blobs <<< "$payload"
    for b in "${blobs[@]}"; do
      [ -f "$tmp/$b" ] && 7z x -y "$tmp/$b" -o"$tmp/_out" >/dev/null 2>&1 || true
    done
    src="$tmp/_out"
  else
    src="$tmp/$sub"
    [ -d "$src" ] || src="$tmp"
  fi

  mkdir -p "$C/$slug"
  find "$src" -maxdepth 1 -mindepth 1 -exec cp -R {} "$C/$slug/" \; 2>/dev/null || true
  rm -rf "$tmp"
  n=$(find "$C/$slug" -type f | wc -l | tr -d ' ')
  exe=$(ls "$C/$slug" 2>/dev/null | grep -iE '\.(exe|bat)$' | head -3 | tr '\n' ' ')
  echo "$(printf '%-8s' "$slug") $(printf '%3d' "$n") files   $exe"
done

# DOS is case-insensitive; these zips are not. Upper-case everything so the
# .BAT/.EXE references inside the games resolve on a case-sensitive host fs.
find "$C" -depth -name '*' | while read -r p; do
  d=$(dirname "$p"); b=$(basename "$p")
  u=$(echo "$b" | tr 'a-z' 'A-Z')
  [ "$b" = "$u" ] || mv -f "$p" "$d/$u" 2>/dev/null || true
done

# COPYFILE_DISABLE: macOS tar otherwise emits an AppleDouble "._NAME" beside
# every file, which lands in the DOS tree as junk the games can see.
COPYFILE_DISABLE=1 tar cf "$G/c.tar" -C "$G" c
echo "wrote $G/c.tar ($(du -h "$G/c.tar" | cut -f1))"
