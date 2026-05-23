# Wiki Schema

## Domain
Plan 9 / 9front desktop UI engineering — WinXP-style window manager, taskbar, launcher,
file manager. Covers Plan 9 graphics APIs, C programming on Plan 9, WinXP visual design
reference, and cross-compile toolchain from macOS.

## Conventions
- File names: lowercase, hyphens (e.g. `draw-api.md`)
- Every page has YAML frontmatter
- Use [[wikilinks]] between pages (min 2 outbound per page)
- Bump `updated` date on every edit
- Add every new page to index.md
- Append every action to log.md

## Frontmatter
```yaml
---
title: Page Title
created: YYYY-MM-DD
updated: YYYY-MM-DD
type: concept | entity | reference | decision | comparison
tags: [from taxonomy]
---
```

## Tag Taxonomy
- plan9: draw, libdraw, devdraw, rio, plumber, rc, fs, 9p
- winxp: luna, titlebar, decorations, taskbar, startmenu, color-scheme
- toolchain: cross-compile, 9c, 9l, plan9port, qemu, vnc
- arch: wm, panel, launcher, filemanager, ipc
- status: wip, done, blocked, decision

## Page Thresholds
- Create when concept appears in 2+ contexts OR is central to implementation
- Don't create for passing mentions
- Split pages over 200 lines

## Update Policy
- Newer info supersedes older — note both if genuinely contradictory
