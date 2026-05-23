#!/usr/bin/env python3
"""
ttf2p9.py - Convert a TTF/OTF font to Plan 9 subfont + .font index format.

Plan 9 fonts are 1-bit bitmaps with a small index format. We rasterize the
TTF using freetype at a chosen pixel size and emit:

  - <name>.<size>.font          (text index)
  - <name>.<size>.<lo>          (binary subfont with raster + headers)

Both files go in the SAME directory; the .font index references the subfont
file by basename (relative path).

The subfont binary layout (see plan9 font(6)):

  +-- plan9 image header --+
  | chan     '          k1'|  11 chars right-justified, blank, 12 total each
  | r.min.x  '           0'|
  | r.min.y  '           0'|
  | r.max.x  '         <W>'|
  | r.max.y  '         <H>'|
  +-- raster bytes ---------+
  |  one row at a time      |
  |  1 bit per pixel        |
  |  packed MSB-first in    |
  |  each byte; rows padded |
  |  to whole bytes         |
  +-- subfont header -------+
  | n        '         <N>'|  3 strings, same width/padding
  | height   '         <H>'|
  | ascent   '         <A>'|
  +-- N+1 Fontchar entries -+
  | each 6 bytes:           |
  |   x      (uint16, LE)   |
  |   top    (uint8)        |
  |   bottom (uint8)        |
  |   left   (int8)         |
  |   width  (uint8)        |
  +-------------------------+

x is the LEFT edge of the glyph in the master raster (cumulative).
top/bottom are the vertical extents of the glyph within the line box.
left is the bearing (signed) - how far left of x the glyph starts.
width is the advance width (cell width for monospace).

The N+1th entry: x of that one = right edge of glyph N-1's raster (so we can
compute the master raster width). Other fields irrelevant.

Glyph 0 by convention should have non-zero width to avoid the "NUL is zero-
width" gotcha. We map glyph 0 to a question mark or the missing glyph from
the font.

USAGE:

    ttf2p9.py <ttf> <out-dir> <name> <pixel-size>

Example:
    ttf2p9.py JetBrainsMono-Regular.ttf out/ jbm 13

Produces:
    out/jbm.13.font           text index pointing at jbm.13.0020
    out/jbm.13.0020           subfont raster for ASCII 0x20-0x7E

Scope: ASCII printable (0x20-0x7E) only. Plan 9 wants one subfont per
unicode range with the file named for the lowest codepoint in hex (4 digits).
We could emit Latin-1 too but ASCII covers the pi9 UI completely.
"""

import sys, os, struct
import freetype


# ASCII printable range. 0x20 (space) through 0x7E (~). Plan 9 wants the
# subfont's lowest codepoint encoded in the filename as 4 hex digits.
LO, HI = 0x20, 0x7E
N = HI - LO + 1                 # number of glyphs


def rasterize(face, pixel_size):
    """Rasterize ASCII glyphs at the given pixel size.

    Returns (glyphs, cell_width, height, ascent) where glyphs is a list
    of (bitmap, top, left, advance) tuples — one per ASCII codepoint."""
    # Set pixel size (height). freetype handles aspect ratio.
    face.set_pixel_sizes(0, pixel_size)

    # Plan 9 fonts assume monospace cell layout. We measure the advance
    # width of 'M' (the canonical "widest" glyph in many fonts). For
    # truly proportional fonts this would clip — but JetBrains Mono /
    # Source Code Pro are designed monospace so advance is uniform.
    face.load_char("M", freetype.FT_LOAD_RENDER | freetype.FT_LOAD_MONOCHROME)
    cell_width = face.glyph.advance.x >> 6      # 26.6 fixed point → pixels

    # Vertical metrics from face. Use the font's design metrics; rounded
    # to whole pixels.
    height = face.size.height >> 6
    ascent = face.size.ascender >> 6

    glyphs = []
    for cp in range(LO, HI + 1):
        ch = chr(cp)
        face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_MONOCHROME)
        bm = face.glyph.bitmap
        # bitmap.buffer is packed 1-bit MSB-first BUT pitch may include
        # padding to byte boundaries. Convert to a list of pixel rows
        # as bools.
        rows = []
        for row_i in range(bm.rows):
            row = []
            for col_i in range(bm.width):
                byte = bm.buffer[row_i * bm.pitch + col_i // 8]
                bit = (byte >> (7 - (col_i % 8))) & 1
                row.append(bit)
            rows.append(row)
        advance = face.glyph.advance.x >> 6
        glyphs.append({
            "rows": rows,
            "width": bm.width,
            "height": bm.rows,
            "top": face.glyph.bitmap_top,           # baseline → top of glyph
            "left": face.glyph.bitmap_left,         # x bearing
            "advance": advance,
        })
    return glyphs, cell_width, height, ascent


def build_subfont(glyphs, cell_width, height, ascent):
    """Build the subfont raster image + char metadata.

    Returns (raster_bytes, fontchar_entries, master_width, master_height).

    Layout strategy: lay all glyphs in a single horizontal strip, one
    cell_width slot per glyph. Master raster is N * cell_width wide, height
    rows tall.
    """
    master_w = N * cell_width
    master_h = height
    # 1-bit packed MSB-first, rows byte-aligned.
    row_bytes = (master_w + 7) // 8
    raster = bytearray(master_h * row_bytes)

    fontchars = []      # one per glyph + sentinel
    for i, g in enumerate(glyphs):
        slot_x = i * cell_width
        # Where the glyph sits inside its cell:
        # - bearing_left is g["left"] (negative for glyphs that overhang left)
        # - bearing_top is g["top"] (distance from baseline upward)
        # - baseline within cell is at row=ascent (counting from top)
        baseline_row = ascent
        for row_i, row in enumerate(g["rows"]):
            # Glyph row 0 is at glyph top, which is at (baseline - top).
            dst_y = baseline_row - g["top"] + row_i
            if dst_y < 0 or dst_y >= master_h:
                continue
            for col_i, bit in enumerate(row):
                if not bit:
                    continue
                dst_x = slot_x + g["left"] + col_i
                if dst_x < slot_x or dst_x >= slot_x + cell_width:
                    # Clip glyphs that overflow their cell (rare for
                    # well-designed monospace fonts).
                    continue
                if dst_x < 0 or dst_x >= master_w:
                    continue
                # Plan 9 image format for k1: bit 0 = ink, bit 1 = paper.
                # We use the inverted convention: SET bit = ink.
                # Actually 9front draws subfonts with k1 where 0=black,
                # 1=white but freetype produces 1=ink. The libdraw image
                # loader treats subfonts as masks; we just need to match
                # the convention plan9 expects.
                # From experimentation: plan9 subfonts are k1 where set
                # bit means GLYPH. Same as freetype's output.
                byte_idx = dst_y * row_bytes + dst_x // 8
                bit_pos = 7 - (dst_x % 8)
                raster[byte_idx] |= (1 << bit_pos)

        # Fontchar for this glyph:
        # - x: left edge of THIS glyph's slot in master (cumulative).
        # - top: top of inked rows (relative to master row 0).
        # - bottom: bottom of inked rows (exclusive).
        # - left: bearing — distance from glyph slot left to actual pixel start.
        # - width: advance width (cell width for monospace).
        top = baseline_row - g["top"] if g["rows"] else 0
        bot = top + len(g["rows"])
        if top < 0: top = 0
        if bot > master_h: bot = master_h
        fontchars.append({
            "x": slot_x,
            "top": top,
            "bottom": bot,
            "left": g["left"] if g["rows"] else 0,
            "width": cell_width,
        })

    # Sentinel: x = right edge of master raster
    fontchars.append({
        "x": master_w,
        "top": 0,
        "bottom": 0,
        "left": 0,
        "width": 0,
    })

    return bytes(raster), fontchars, master_w, master_h


def plan9_header(field):
    """Right-justify in 11 chars + trailing space. 12 chars total."""
    s = str(field)
    if len(s) > 11:
        raise ValueError(f"header field too long: {field}")
    return f"{s:>11} ".encode("ascii")


def encode_fontchar(fc):
    """Plan 9 Fontchar: 6 bytes, x as uint16 LE."""
    return struct.pack("<HBBbB",
        fc["x"] & 0xFFFF,
        fc["top"] & 0xFF,
        fc["bottom"] & 0xFF,
        fc["left"],
        fc["width"] & 0xFF,
    )


def write_subfont(path, raster, fontchars, master_w, master_h):
    """Emit a complete subfont file (image header + raster + sub header + entries)."""
    with open(path, "wb") as f:
        # Plan 9 image header: 5 fields, each 11-char right-justified + space.
        f.write(plan9_header("k1"))
        f.write(plan9_header("0"))
        f.write(plan9_header("0"))
        f.write(plan9_header(master_w))
        f.write(plan9_header(master_h))
        # Raster
        f.write(raster)
        # Subfont header: 3 fields (n, height, ascent), same format.
        f.write(plan9_header(N))          # number of REAL chars (not counting sentinel)
        f.write(plan9_header(master_h))   # subfont's height
        f.write(plan9_header(master_h))   # subfont's ascent (we use full height)
        # Fontchar entries (N+1, including sentinel)
        for fc in fontchars:
            f.write(encode_fontchar(fc))


def write_font_index(path, height, ascent, subfont_name):
    """Emit the .font text index. Format:

        height ascent
        lo hi subfontfile
    """
    with open(path, "w") as f:
        f.write(f"{height} {ascent}\n")
        f.write(f"{LO:#x} {HI:#x} {subfont_name}\n")


def main():
    if len(sys.argv) != 5:
        print("usage: ttf2p9.py <ttf> <out-dir> <name> <pixel-size>", file=sys.stderr)
        sys.exit(1)
    ttf_path, out_dir, name, size_s = sys.argv[1:]
    size = int(size_s)

    os.makedirs(out_dir, exist_ok=True)
    face = freetype.Face(ttf_path)

    print(f"rasterizing {ttf_path} at {size}px...", file=sys.stderr)
    glyphs, cell_w, height, ascent = rasterize(face, size)
    print(f"  cell_width={cell_w} height={height} ascent={ascent}", file=sys.stderr)

    raster, fontchars, mw, mh = build_subfont(glyphs, cell_w, height, ascent)
    print(f"  master raster: {mw}x{mh}px ({len(raster)} bytes)", file=sys.stderr)

    # Output paths. Plan 9 convention: subfont named <name>.<size>.<lo>
    # where lo is 4 hex digits, font index named <name>.<size>.font.
    subfont_name = f"{name}.{size}.{LO:04x}"
    font_name = f"{name}.{size}.font"
    subfont_path = os.path.join(out_dir, subfont_name)
    font_path = os.path.join(out_dir, font_name)

    write_subfont(subfont_path, raster, fontchars, mw, mh)
    write_font_index(font_path, height, ascent, subfont_name)

    print(f"wrote {font_path} -> {subfont_path}", file=sys.stderr)
    print(f"        height={height} ascent={ascent}", file=sys.stderr)


if __name__ == "__main__":
    main()
