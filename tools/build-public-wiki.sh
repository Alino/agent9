#!/bin/bash
# build-public-wiki.sh — assemble the public docs/wiki/ tree from
# the internal Obsidian-backed wiki at ../wiki (symlink target).
#
# Drops: entities/, queries/ (Obsidian dataview), log.md (internal
# session-by-session log with personal references).
# Keeps: concepts/, raw/, assets/, blog/, comparisons/, index.md,
# SCHEMA.md.
set -e
cd "$(dirname "$0")/.."

SRC="$(readlink wiki || echo wiki)"
DST="docs/wiki"

if [ ! -d "$SRC" ]; then
  echo "error: $SRC not found. Is the wiki symlink broken?"
  exit 1
fi

rm -rf "$DST"
mkdir -p "$DST"

for d in concepts raw assets blog comparisons; do
  if [ -d "$SRC/$d" ]; then
    cp -R "$SRC/$d" "$DST/"
  fi
done

# Top-level files
for f in index.md SCHEMA.md; do
  if [ -f "$SRC/$f" ]; then
    cp "$SRC/$f" "$DST/"
  fi
done

# Personal-content denylist — these files contain my chat transcripts /
# personal research and don't belong in the public wiki.
DENYLIST=(
  "raw/articles/claude-share-llm-re-porting-asahi-2026-05.md"
)
for f in "${DENYLIST[@]}"; do
  rm -f "$DST/$f"
done

# Sanitize log.md if you want to ship a redacted version: skipped for
# v0.1. The full log lives in the private wiki only.

# Strip Obsidian-specific syntax that won't render on GitHub.
# - [[wikilink]] → keep as-is (GitHub renders these as broken links,
#   but readers can still see the intent). Future TODO: rewrite to
#   real relative paths.

# Write a public wiki entrypoint
cat > "$DST/README.md" <<'EOF'
# agent9 wiki

Architecture deep-dives, per-component design notes, and the long
form of the project's "we tried X and it didn't work because Y"
discoveries.

Start here:

- [`index.md`](index.md) — table of contents
- [`concepts/pi9-architecture.md`](concepts/pi9-architecture.md) — pi9 design
- [`concepts/vt-architecture.md`](concepts/vt-architecture.md) — vts/vtwin design
- [`concepts/plan9-namespaces-for-agents.md`](concepts/plan9-namespaces-for-agents.md) — why namespaces matter for LLM agents
- [`blog/`](blog/) — long-form posts about the project

This wiki was authored in Obsidian (with `[[wikilinks]]` and YAML
frontmatter). Most pages render fine on GitHub. Wikilinks render as
broken links; that's expected for v0.1 and will be fixed when the
docs get a proper static-site build.

The development log (chronological "what we did and learned today"
notes) is not in the public wiki — those notes are internal session
history and don't add value outside the project.
EOF

echo "built $DST"
echo "size:"
du -sh "$DST"
echo "files:"
find "$DST" -type f | wc -l
