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
import os, struct, sys

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

    # The a.out header packs text/data/bss as 32-bit big-endian. clang (~150 MB)
    # is well under 4 GB, but a segment that overflows would silently truncate and
    # produce a corrupt binary — fail loudly instead.
    for nm, val in (('text', len(text)), ('data', len(data)), ('bss', bss)):
        if val >= (1 << 32):
            sys.exit(f"{nm} segment 0x{val:x} >= 4 GB: does not fit the 32-bit "
                     f"a.out size field (would truncate)")

    # --- Plan 9 amd64 symbol table (so acid can name frames) ---
    # Parse the ELF .symtab/.strtab and emit Plan 9 syms: 8-byte big-endian value,
    # one type byte (0x80|ascii), NUL-terminated name. Text funcs -> 'T', data/bss
    # objects -> 'D'/'B'. Enough for acid backtraces.
    syms = build_symtab(elf, TEXTVA + len(text))

    hdr  = struct.pack('>8I', S_MAGIC, len(text), len(data), bss, len(syms),
                       e_entry & 0xffffffff, 0, 0)
    hdr += struct.pack('>Q', e_entry)
    out = hdr + text + data + syms
    # Atomic write: a plain open('wb') truncates the target FIRST, so a kill mid-write
    # (run9 timeout / OOM) would leave a 0-byte a.out that `deliver`/`run9` then ships
    # (the 0-byte-binary incident). Write a sibling .tmp and os.replace() it — the final
    # path is always either the previous good a.out or the complete new one.
    tmp = sys.argv[2] + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(out)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, sys.argv[2])
    print(f"{sys.argv[2]}: {len(out)} bytes  text={len(text)} data={len(data)} "
          f"bss={bss} syms={len(syms)} entry=0x{e_entry:x}")

def build_symtab(elf, data_end_va):
    e_shoff = u64(elf, 0x28)
    e_shentsize = u16(elf, 0x3a)
    e_shnum = u16(elf, 0x3c)
    symtab = strtab = None
    for i in range(e_shnum):
        o = e_shoff + i*e_shentsize
        if u32(elf, o+4) == 2:            # SHT_SYMTAB
            symtab = (u64(elf, o+24), u64(elf, o+32), u32(elf, o+40))  # off,size,link
    if not symtab:
        return b''
    so, ssz, link = symtab
    lo = e_shoff + link*e_shentsize
    stro, strsz = u64(elf, lo+24), u64(elf, lo+32)
    out = bytearray()
    n = ssz // 24
    for i in range(n):
        e = so + i*24
        st_name = u32(elf, e+0)
        st_info = elf[e+4]
        st_shndx = u16(elf, e+6)
        st_value = u64(elf, e+8)
        typ = st_info & 0xf               # 1=OBJECT 2=FUNC
        if typ not in (1, 2) or st_shndx == 0 or st_value == 0:
            continue
        end = elf.index(b'\x00', stro+st_name)
        name = elf[stro+st_name:end]
        if not name:
            continue
        ch = 'T' if typ == 2 else ('B' if st_value >= data_end_va else 'D')
        out += struct.pack('>Q', st_value) + bytes([0x80 | ord(ch)]) + name + b'\x00'
    return bytes(out)

if __name__ == '__main__':
    main()
