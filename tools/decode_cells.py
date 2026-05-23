#!/usr/bin/env python3
import sys, struct

if len(sys.argv) < 2:
    print("usage: decode_cells.py <file>", file=sys.stderr)
    sys.exit(1)

data = open(sys.argv[1], "rb").read()
print(f"total bytes: {len(data)}")

if len(data) < 22:
    print(f"file too short")
    sys.exit(1)

magic = struct.unpack_from("<I", data, 0)[0]
ver = struct.unpack_from("<H", data, 4)[0]
rows = struct.unpack_from("<H", data, 6)[0]
cols = struct.unpack_from("<H", data, 8)[0]
ncells = struct.unpack_from("<I", data, 10)[0]
cur_row = struct.unpack_from("<H", data, 14)[0]
cur_col = struct.unpack_from("<H", data, 16)[0]
vis = data[18]

if magic != 0x76746331:
    print(f"bad magic {magic:#010x}")
    sys.exit(1)

print(f"magic=0x{magic:08x} ver={ver} rows={rows} cols={cols} ncells={ncells} cur=({cur_row},{cur_col}) vis={vis}")

grid = [[' ' for _ in range(cols)] for _ in range(rows)]

off = 22
for i in range(ncells):
    if off + 12 > len(data):
        print(f"truncated at cell {i}")
        break
    row = struct.unpack_from("<H", data, off)[0]
    col = struct.unpack_from("<H", data, off+2)[0]
    rune = struct.unpack_from("<I", data, off+4)[0]
    fg = data[off+8]
    bg = data[off+9]
    attrs = data[off+10]
    off += 12
    if 0 <= row < rows and 0 <= col < cols:
        try:
            ch = chr(rune) if 32 <= rune < 127 else '.'
        except:
            ch = '?'
        grid[row][col] = ch

print("---")
for r, row in enumerate(grid):
    line = ''.join(row).rstrip()
    if line:
        print(f"{r:2}|{line}")
print("---")
print(f"cursor at ({cur_row},{cur_col})")
