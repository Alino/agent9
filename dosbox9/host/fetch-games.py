#!/usr/bin/env python3
"""fetch-games.py — download the DOS games used to validate dosbox9, from the
Internet Archive.

Every entry here is a shareware episode or a publisher-released freeware title
— the episodes that were always legal to redistribute. Two traps worth knowing,
because archive.org's MS-DOS library mixes shareware with abandonware:

  - Wolfenstein 3D: take `w3d-box` (1wolf14.zip, shareware v1.4, .WL1 data).
    The `wolfenstein-3d` item is .WL6 — the full REGISTERED game.
  - Prince of Persia: deliberately absent. `msdos_Prince_of_Persia_1990` is
    commercial abandonware, not shareware.

If you add a title, check what you actually got before claiming it's free.

    python3 fetch-games.py /tmp/dosgames
"""
import json, os, subprocess, sys, urllib.request

# (archive.org identifier, zip name, short slug, what it exercises)
#
# Prefer items that ship the game already extracted (they're the ones archive's
# in-browser emulator runs). Items whose zip is a DEICE/INSTALL floppy image or
# an .iso need an installer run inside DOSBox first — avoid where possible.
GAMES = [
    ("keen1-sw",        "keen1.zip",    "keen1",  "EGA 16-colour platformer, PC speaker"),
    ("w3d-box",         "1wolf14.zip",  "wolf3d", "VGA raycaster, AdLib (shareware v1.4, .WL1)"),
    ("msdos_Jill_of_the_Jungle_1992", "Jill_of_the_Jungle_1992.zip", "jill", "EGA platformer"),
    ("Raptor-sw1",      "raptor.zip",   "raptor", "VGA vertical shooter"),
    ("Bs-aog-sw1",      "bstone.zip",   "blake",  "VGA raycaster"),
    ("duke-nukem2-sw",  "duke2.zip",    "duke2",  "EGA parallax platformer"),
    ("msdos_Hocus_Pocus_1994", "Hocus_Pocus_1994.zip", "hocus", "VGA platformer"),
    ("msdos_Major_Stryker_1993", "Major_Stryker_1993.zip", "stryker", "EGA scroller"),
    ("CosmosCosmicAdventure", "cosmo.zip", "cosmo", "EGA parallax scroller"),
    ("DoomsharewareEpisode", "doom19s.zip", "doom",  "VGA 320x200 chunky, the big one"),
    ("msdos_Commander_Keen_4_-_Secret_of_the_Oracle_1991", "", "keen4", "EGA smooth scroller"),
    ("biomenace1-sw",    "bmenace.zip",  "bmenace", "EGA run-and-gun"),
    ("crystal-caves",    "crystal-caves.zip", "caves", "EGA platformer"),
    ("halloween-harry",  "Halloween_Harry.zip", "harry", "VGA platformer"),
]


def meta(ident):
    with urllib.request.urlopen(f"https://archive.org/metadata/{ident}", timeout=30) as r:
        return json.load(r)


def main(dest):
    os.makedirs(dest, exist_ok=True)
    ok, bad = [], []
    for ident, zipname, slug, why in GAMES:
        out = os.path.join(dest, f"{slug}.zip")
        if os.path.exists(out) and os.path.getsize(out) > 1000:
            print(f"have {slug}")
            ok.append(slug)
            continue
        try:
            m = meta(ident)
            names = [f["name"] for f in m.get("files", [])]
            # prefer the declared name; else the biggest .zip in the item
            pick = zipname if zipname in names else None
            if not pick:
                zips = [f for f in m.get("files", [])
                        if f["name"].lower().endswith(".zip")
                        and "archive.torrent" not in f["name"]]
                if not zips:
                    raise RuntimeError("no zip in item")
                pick = max(zips, key=lambda f: int(f.get("size") or 0))["name"]
            url = f"https://archive.org/download/{ident}/{urllib.parse.quote(pick)}"
            subprocess.run(["curl", "-sL", "-m", "180", "-o", out, url], check=True)
            if os.path.getsize(out) < 1000:
                raise RuntimeError(f"tiny file ({os.path.getsize(out)}B)")
            print(f"OK   {slug:8s} {os.path.getsize(out):>9,}B  {pick}  ({why})")
            ok.append(slug)
        except Exception as e:
            print(f"FAIL {slug:8s} {ident}: {e}")
            if os.path.exists(out):
                os.remove(out)
            bad.append(slug)
    print(f"\n{len(ok)} ok, {len(bad)} failed: {bad}")


if __name__ == "__main__":
    import urllib.parse
    main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/dosgames")
