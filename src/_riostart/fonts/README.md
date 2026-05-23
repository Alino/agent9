# src/_riostart/fonts

TrueType fonts shipped alongside the agent9 boot rc. Copied verbatim
into the 9front VM at `/lib/font/ttf/` so that fontsrv(8) can synthesise
antialiased subfonts on demand (see `wiki/concepts/vtwin-typography.md`).

## Contents

| File                       | Family       | License | Source |
|----------------------------|--------------|---------|--------|
| `Inconsolata-Regular.ttf`  | Inconsolata  | OFL 1.1 | https://github.com/googlefonts/Inconsolata |

SHA-256 of bundled files (verify with `shasum -a 256`):

    ab56ea18c5c24d2b909261f0c63a14f9576dfabaf2e9ebd353062aa4149cefc7  Inconsolata-Regular.ttf

## Install into the VM

From the VM (after sources are pushed via `hget`):

    mkdir -p /lib/font/ttf
    hget http://10.0.2.2:8765/src/_riostart/fonts/Inconsolata-Regular.ttf > \
        /lib/font/ttf/Inconsolata-Regular.ttf

Or use the helper that does the same in one go and verifies fontsrv:

    hget http://10.0.2.2:8765/src/_riostart/install-fonts.rc | rc

`src/_riostart/profile` then calls `9fs fontsrv` at terminal boot and
sets `$font=/n/font/Inconsolata/16a/font`.

## Why Inconsolata 16a as the default

- OFL — no license friction for shipping in our repo.
- Plan 9 community favourite (used in 9front docs, acme tutorials).
- 16px antialiased renders as ~8×16 cells = roughly 95×33 grid in a
  760×540 vtwin window, leaving room for mxio's 22 px titlebar.
- No ligatures, no shaping requirements — same visual footprint as the
  defaultfont it replaces, just antialiased and modern.

Switch by editing `font=` in `src/_riostart/profile`. Other reasonable
picks (JetBrains Mono, IBM Plex Mono, Iosevka) are in
`wiki/concepts/vtwin-typography.md`.

## Adding more fonts

1. Drop the `.ttf` here. OFL/Apache/SIL-compatible only — no commercial
   redistribution.
2. Add a row to the table above and append the SHA-256.
3. Extend `install-fonts.rc` to fetch the new file.

fontsrv discovers any `.ttf` in `/lib/font/ttf/` automatically.
