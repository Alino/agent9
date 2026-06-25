#!/usr/bin/env python3
"""
elf2aout — convert a statically-linked x86_64 ELF (laid out by cc9/test/plan9.ld)
into a Plan 9 amd64 a.out.

Plan 9 a.out (amd64): 40-byte big-endian header
  magic(0x8A97) text data bss syms entry spsz pcsz  (8x uint32) + uint64 entry
File: [40B hdr][text][data].  Kernel maps text at TEXTVA=0x200028 and the data
segment at DATAVA = roundup(UTZERO+HDR+textsize, 0x200000); bss is zero-filled
after data. The linker must place text@0x200028 and data@DATAVA so absolute refs
resolve correctly (verified: small-text binary -> data@0x400000).

Usage: elf2aout.py in.elf out.aout
"""
import struct, sys

S_MAGIC = 0x8A97
UTZERO  = 0x200000
HDRSZ   = 40
TEXTVA  = UTZERO + HDRSZ          # 0x200028
DATA_ALIGN = 0x200000

def u16(b,o): return struct.unpack_from('<H', b, o)[0]
def u32(b,o): return struct.unpack_from('<I', b, o)[0]
def u64(b,o): return struct.unpack_from('<Q', b, o)[0]

def main():
    elf = open(sys.argv[1], 'rb').read()
    assert elf[:4] == b'\x7fELF' and elf[4] == 2 and elf[5] == 1, 'need LE 64-bit ELF'
    e_entry = u64(elf, 0x18)
    e_phoff = u64(elf, 0x20)
    e_phentsize = u16(elf, 0x36)
    e_phnum = u16(elf, 0x38)

    text_seg = None   # R+X load
    data_seg = None   # R+W load
    for i in range(e_phnum):
        o = e_phoff + i*e_phentsize
        if u32(elf, o) != 1:          # PT_LOAD only
            continue
        flags  = u32(elf, o+0x04)
        seg = dict(off=u64(elf,o+0x08), vaddr=u64(elf,o+0x10),
                   filesz=u64(elf,o+0x20), memsz=u64(elf,o+0x28), flags=flags)
        if flags & 0x1:               # executable -> text
            text_seg = seg
        elif flags & 0x2:             # writable -> data+bss
            data_seg = seg
    assert text_seg, 'no R+X (text) segment'

    # --- text: image based at TEXTVA; linker may bump a few bytes (pad with 0) ---
    tlo = text_seg['vaddr']
    assert tlo >= TEXTVA, f'text vaddr 0x{tlo:x} < 0x{TEXTVA:x}'
    text = bytearray((tlo - TEXTVA) + text_seg['filesz'])
    text[tlo-TEXTVA : tlo-TEXTVA+text_seg['filesz']] = \
        elf[text_seg['off'] : text_seg['off']+text_seg['filesz']]
    text = bytes(text)

    # --- data + bss ---
    if data_seg:
        expect = (TEXTVA + len(text) + DATA_ALIGN - 1) & ~(DATA_ALIGN - 1)
        if data_seg['vaddr'] != expect:
            sys.exit(f"data vaddr 0x{data_seg['vaddr']:x} != kernel-expected "
                     f"0x{expect:x} (text too big for the 0x200000 round?)")
        data = elf[data_seg['off'] : data_seg['off']+data_seg['filesz']]
        bss  = data_seg['memsz'] - data_seg['filesz']
    else:
        data, bss = b'', 0

    hdr  = struct.pack('>8I', S_MAGIC, len(text), len(data), bss, 0,
                       e_entry & 0xffffffff, 0, 0)
    hdr += struct.pack('>Q', e_entry)
    out = hdr + text + data
    open(sys.argv[2], 'wb').write(out)
    print(f"{sys.argv[2]}: {len(out)} bytes  text={len(text)} data={len(data)} "
          f"bss={bss} entry=0x{e_entry:x}")

if __name__ == '__main__':
    main()
