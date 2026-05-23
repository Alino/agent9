---
title: WinXP Visual Specification
created: 2026-05-15
updated: 2026-05-16
type: reference
tags: [winxp, luna, titlebar, decorations, color-scheme]
---

# WinXP Visual Specification (Luna theme)

Exact values for implementing the WinXP appearance on Plan 9.

## Colors (Plan 9 0xRRGGBBFF format)

### Titlebar — active window
```
Gradient start (left edge): 0x003399FF   (#003399 — dark luna blue)
Gradient mid:               0x2B7DD4FF   (#2B7DD4)
Gradient end (right edge):  0x5BAAEDFF   (#5BAAED — light)
Text:                       0xFFFFFFFF   (white)
```
Plan 9 libdraw has no native gradient — we simulate it: N vertical `draw()` strips
with interpolated color (16 strips = smooth enough, ~1ms overhead).

### Titlebar — inactive window
```
Color:  0x7A96DFFF   (#7A96DF — faded blue)
Text:   0xD8E4F8FF   (light, not white)
```

### Decoration buttons (close/min/max)
```
Button background:  0xD73B3BFF  close red (hover), otherwise gradient
Button border:      0x00000066  subtle shadow
Icon (X, _, □):    0xFFFFFFFF  white
Button width:       21px
Button height:      21px  (centered in 22px titlebar)
Button margin:      1px apart, 2px from right edge
```

### Panel (taskbar)
```
Background:        0x245EDCFF   (luna blue — more saturated than titlebars)
Start button bg:   0x3A8A3AFF   (green, characteristic of XP)
Start button text: 0xFFFFFFFF
Window button bg:  0x3367C0FF   (active/focused window = darker)
Window button bg:  0x2B5AB5FF   (inactive)
Clock text:        0xFFFFFFFF
Height:            30px
```

### Desktop background
```
Default XP wallpaper green: 0x2A7A29FF
Without wallpaper, solid:   0x3A6EA5FF  (luna blue-grey)
```

### Dialog / app window background
```
0xECE9D8FF   (#ECE9D8 — classic XP grey)
```

## Dimensions

```
Titlebar height:    22px
Border width:       3px  (left, right, bottom edges)
Button width:       21px
Button height:      21px
Panel height:       30px
Start button width: 100px  (text "Start" + windows logo)
Window button:      min 100px, max 160px (shrinks when many windows)
Clock width:        ~60px
```

## Fonts

Plan 9 has bitmap fonts in `/lib/font/bit/`. For WinXP feel:
```
/lib/font/bit/lucidasans/unicode.10  -- closest to Tahoma 8pt
/lib/font/bit/lucidasans/unicode.12  -- for titlebars (no bold variant)
```
Bold text is simulated: draw the string twice with a 1px offset (primitive bold).

## Decoration Buttons — Shapes (in Plan 9 draw primitives)

### Close [X]
```c
// X = two diagonal lines across the button, white, 2px thick
// Plan 9 line() has no thickness param — draw 3 parallel 1px lines
line(dst, Pt(x+4,y+4), Pt(x+16,y+16), 0, 0, 1, white, ZP);
line(dst, Pt(x+4,y+16), Pt(x+16,y+4), 0, 0, 1, white, ZP);
```

### Minimize [_]
```c
// Underline at the bottom
draw(dst, Rect(x+4, y+15, x+17, y+17), white, nil, ZP);
```

### Maximize [□]
```c
// Rectangle outline, 1px border
border(dst, Rect(x+4, y+5, x+17, y+16), 1, white, ZP);
```

## Gradient Implementation in libdraw

```c
void drawtitlebar(Image *dst, Rectangle r, int active) {
    ulong colors[3] = active
        ? (ulong[]){0x003399FF, 0x2B7DD4FF, 0x5BAAEDFF}
        : (ulong[]){0x7A96DFFF, 0x7A96DFFF, 0x7A96DFFF};
    int w = r.max.x - r.min.x;
    int steps = 16;
    for(int i = 0; i < steps; i++) {
        int x0 = r.min.x + (w * i / steps);
        int x1 = r.min.x + (w * (i+1) / steps);
        // interpolate color between colors[0] and colors[2]
        ulong c = lerpcolor(colors[0], colors[2], i, steps);
        Image *col = allocimage(display, Rect(0,0,1,1), RGB24, 1, c);
        draw(dst, Rect(x0, r.min.y, x1, r.max.y), col, nil, ZP);
        freeimage(col);
    }
}
```

## See Also

- [[draw-api]] — libdraw primitives: draw(), border(), string(), line()
- [[mxio-design]] — where and how these values are used
- [[rio-architecture]] — what we are modifying
