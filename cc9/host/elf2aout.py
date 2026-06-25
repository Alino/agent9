#!/usr/bin/env python3
"""
elf2aout — convert a statically-linked x86_64 ELF executable into a Plan 9
amd64 a.out. Phase-1 minimal version: assumes the linker placed the loadable
image so that text begins at TEXTVA (0x200028 = UTZERO+HEADER) and there is a
single contiguous loadable image (use `ld.lld --no-rosegment` so .rodata rides
in the text segment; no writable .data yet -> data/bss = 0).

Plan 9 a.out (amd64): 40-byte big-endian header
  magic(0x8A97) text data bss syms entry spsz pcsz  (8x uint32) + uint64 entry
File layout: [40B hdr][text][data][syms][pcsz]; loads vaddr = UTZERO + file_off.

Usage: elf2aout.py in.elf out.aout
"""
import struct, sys

S_MAGIC = 0x8A97
UTZERO  = 0x200000
HDRSZ   = 40
TEXTVA  = UTZERO + HDRSZ   # 0x200028

def u16(b,o): return struct.unpack_from('<H', b, o)[0]
def u32(b,o): return struct.unpack_from('<I', b, o)[0]
def u64(b,o): return struct.unpack_from('<Q', b, o)[0]

def main():
    elf = open(sys.argv[1], 'rb').read()
    assert elf[:4] == b'\x7fELF', 'not ELF'
    assert elf[4] == 2, 'not 64-bit'
    assert elf[5] == 1, 'not little-endian'
    e_entry = u64(elf, 0x18)
    e_phoff = u64(elf, 0x20)
    e_phentsize = u16(elf, 0x36)
    e_phnum = u16(elf, 0x38)

    # Collect PT_LOAD segments (type==1).
    loads = []
    for i in range(e_phnum):
        o = e_phoff + i*e_phentsize
        p_type = u32(elf, o)
        if p_type != 1:
            continue
        p_off  = u64(elf, o+0x08)
        p_vaddr= u64(elf, o+0x10)
        p_filesz=u64(elf, o+0x20)
        p_memsz =u64(elf, o+0x28)
        p_flags = u32(elf, o+0x04)
        loads.append((p_vaddr, p_off, p_filesz, p_memsz, p_flags))
    loads.sort()
    assert loads, 'no PT_LOAD'

    # Build the flat image based at the Plan 9 text vaddr (TEXTVA). The linker
    # may place text a few bytes above TEXTVA (alignment); those bytes become
    # leading zero padding in the a.out text segment (never executed; entry
    # points past them). Absolute refs stay correct since each segment lands at
    # its own vaddr.
    lo = min(v for v, *_ in loads)
    if lo < TEXTVA:
        sys.exit(f'lowest LOAD vaddr 0x{lo:x} < TEXTVA 0x{TEXTVA:x}')
    hi = max(v + memsz for v, _, _, memsz, _ in loads)
    img = bytearray(hi - TEXTVA)
    for v, off, filesz, memsz, flags in loads:
        img[v-TEXTVA : v-TEXTVA+filesz] = elf[off:off+filesz]
    text = bytes(img)
    # No separate writable data segment in this minimal path (script discards it).
    data_sz = 0
    bss_sz  = 0
    text_sz = len(text)

    hdr  = struct.pack('>8I', S_MAGIC, text_sz, data_sz, bss_sz, 0,
                       e_entry & 0xffffffff, 0, 0)
    hdr += struct.pack('>Q', e_entry)
    out = hdr + text
    open(sys.argv[2], 'wb').write(out)
    print(f"{sys.argv[2]}: {len(out)} bytes  text={text_sz} entry=0x{e_entry:x} "
          f"(LOAD base 0x{lo:x})")

if __name__ == '__main__':
    main()
