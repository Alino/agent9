#!/usr/bin/env python3
"""
Build a minimal Plan 9 amd64 a.out from the HOST (no kencc) that just does
exits(nil) — the smallest possible proof of the host->9front toolchain spine.

Plan 9 amd64 facts (decoded from a real kencc binary + 9syscall/mkfile):
  - 40-byte big-endian header: 8x int32 Exec fields + 8-byte 64-bit entry.
  - amd64 magic S_MAGIC = 0x8A97 (HDR_MAGIC|... ).
  - load: vaddr = 0x200000 + file_offset; header at 0x200000, text right after.
  - syscall ABI: number in RBP (RARG), args on the stack above a retpc slot,
    SYSCALL instruction. EXITS = 8.
"""
import struct, sys

S_MAGIC = 0x8A97
UTZERO  = 0x200000
HDRSZ   = 40
EXITS   = 8

# freestanding entry. Kernel jumps here. Exact bytes the Plan 9 assembler (6a)
# emitted for the equivalent asm (from a runs-clean reference binary):
#   sub rsp,16 ; mov qword[rsp+8],0 ; mov rbp,8(EXITS) ; syscall ; add rsp,16 ; ret
# arg0 (nil exits-message) lands at [rsp+8]; syscall number in rbp (RARG).
text = bytes([
    0x48, 0x83, 0xEC, 0x10,                          # sub  $0x10, %rsp
    0x48, 0xC7, 0x44, 0x24, 0x08, 0x00,0x00,0x00,0x00, # mov  $0, 8(%rsp)
    0x48, 0xC7, 0xC5, EXITS,0,0,0,                    # mov  $8, %rbp  (EXITS)
    0x0F, 0x05,                                      # syscall
    0x48, 0x83, 0xC4, 0x10,                          # add  $0x10, %rsp
    0xC3,                                            # ret
])

entry = UTZERO + HDRSZ                   # first text byte
# Exec: magic,text,data,bss,syms,entry,spsz,pcsz  (big-endian int32)
hdr  = struct.pack('>8I', S_MAGIC, len(text), 0, 0, 0, entry & 0xffffffff, 0, 0)
hdr += struct.pack('>Q', entry)          # 64-bit entry expansion

out = hdr + text
path = sys.argv[1] if len(sys.argv) > 1 else 'min.aout'
with open(path, 'wb') as f:
    f.write(out)
print(f"wrote {path}: {len(out)} bytes (hdr {HDRSZ} + text {len(text)}), entry=0x{entry:x}")
print("hex:", out.hex())
