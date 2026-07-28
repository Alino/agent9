# tile9

Global window-management hotkeys for 9front. Magnet/yabai-style keyboard
tiling on rio, in one C file and about 250 lines.

```
pac9 install tile9
```

## Keys

| Chord | Action |
|---|---|
| `Ctrl+Alt+Left` / `Ctrl+Alt+J` | left half |
| `Ctrl+Alt+Right` / `Ctrl+Alt+L` | right half |
| `Ctrl+Alt+Up` | top half |
| `Ctrl+Alt+Down` / `Ctrl+Alt+K` | bottom half |
| `Ctrl+Alt+Enter` | maximize (not fullscreen) |
| `Alt+1` … `Alt+6` | launch an app |

Tiles keep 10px of padding from every screen edge and a 10px gap from each
other. If an `xena-panel` taskbar is running, the strip it occupies is kept
clear; on a stock rio with no taskbar, tiles run to the edge.

There is deliberately no `Ctrl+Alt+I` for "top half". Ctrl+I *is* TAB, and
mxio's Alt+Tab handler consumes it before tile9 can see it. Use the arrow.

## Running it

tile9 must run inside the window system's namespace — an ssh or listen1 shell
is not, and there `/dev/kbdtap` does not exist. Start it from your `riostart`:

```rc
/amd64/bin/tile9 >/dev/null >[2]/dev/null &
```

Only one program at a time may hold rio's keyboard tap, so a second tile9
exits with `file in use`.

## Configuring the launchers

`Alt+1..6` default to `mothra`, `rc`, `acme`, `sam`, `pi9`, `stats`. Override
any of them in `$home/lib/tile9`, one `<digit> <rc command>` per line:

```
1 window mothra
2 window /bin/rc
3 window acme
6 window nedmail
```

Anything not listed keeps its default; a digit with nothing bound is passed
through to the focused window instead of being swallowed.

## How it works

Plan 9 delivers keystrokes only to the focused window, so a program cannot
normally see a system-wide chord. rio solves this with **`/dev/kbdtap`**: open
it and rio hands you every keyboard message and delivers only the ones you
write back. Acting on a chord and declining to write it back *is* the swallow.
Geometry is applied by writing `resize -r x0 y0 x1 y1` to
`/dev/wsys/<id>/wctl`, and the focused window is whichever one rio reports as
`current`.

So this is not a window manager and not a patched rio — it is an ordinary
program using two files rio already serves. It works the same on stock 9front
rio and on mxio.

The protocol has sharp edges (the character event under Ctrl is *not* the key
you pressed; the focus notification is edge-triggered and remembered across
tap opens). `docs/wiki/concepts/tile9-hotkeys.md` writes them up.

## Testing

```
tile9 -t     # grid self-test, no window system needed
tile9 -d     # dump every keyboard message and every resize written
```
