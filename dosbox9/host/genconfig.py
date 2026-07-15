#!/usr/bin/env python3
"""Generate config.h from DOSBox's config.h.in for the cc9 -> 9front target.

This is what autoconf would do, minus autoconf: substitute the `#undef X` lines
and keep config.h.in's typedef/macro tail verbatim (that tail defines Bitu,
Bit32u, GCC_ATTRIBUTE, INLINE... — hand-writing config.h loses it).
"""
import re, sys

# ponytail: everything optional is off. Turn one on when a game needs it.
DEFS = {
    # toolchain capabilities (clang)
    "C_HAS_ATTRIBUTE": 1,
    "C_HAS_BUILTIN_EXPECT": 1,
    "C_ATTRIBUTE_ALWAYS_INLINE": 1,
    "C_CORE_INLINE": 1,
    "C_UNALIGNED_MEMORY": 1,
    "C_TARGETCPU": "X86_64",
    # cores: portable interpreter only. C_DYNREC needs W^X (wxallow + exec
    # pool) and a working mprotect; add when the interpreter proves too slow.
    "C_FPU": 1,
    # sizes: cc9 on Plan 9 amd64 is SysV LP64
    "SIZEOF_INT_P": 8,
    "SIZEOF_UNSIGNED_CHAR": 1,
    "SIZEOF_UNSIGNED_SHORT": 2,
    "SIZEOF_UNSIGNED_INT": 4,
    "SIZEOF_UNSIGNED_LONG": 8,
    "SIZEOF_UNSIGNED_LONG_LONG": 8,
    # headers cc9 provides
    "HAVE_STDINT_H": 1, "HAVE_STDLIB_H": 1, "HAVE_STRING_H": 1,
    "HAVE_STRINGS_H": 1, "HAVE_MEMORY_H": 1, "HAVE_SYS_TYPES_H": 1,
    "HAVE_SYS_STAT_H": 1, "HAVE_UNISTD_H": 1, "HAVE_INTTYPES_H": 1,
    "HAVE_SYS_SOCKET_H": 1, "HAVE_NETINET_IN_H": 1, "STDC_HEADERS": 1,
    "DIRENT_HAS_D_TYPE": 1, "HAVE_REALPATH": 1,
    "PACKAGE": '"dosbox"', "PACKAGE_NAME": '"dosbox"',
    "PACKAGE_TARNAME": '"dosbox"', "PACKAGE_VERSION": '"0.74-3"',
    "PACKAGE_STRING": '"dosbox 0.74-3"', "PACKAGE_BUGREPORT": '""',
    "PACKAGE_URL": '""', "VERSION": '"0.74-3"',
}
# Left undefined on purpose (each would drag in a dependency or a broken
# assumption): LINUX MACOSX OS2 BSD, C_DYNAMIC_X86 C_DYNREC, C_FPU_X86 (x87
# inline asm), C_HAVE_MPROTECT (cc9's mprotect is a no-op), C_OPENGL C_IPX
# C_MODEM C_DIRECTSERIAL C_SDL_SOUND C_SSHOT C_DEBUG C_HEAVY_DEBUG
# C_SET_PRIORITY C_X11_XKB HAVE_ALSA HAVE_PWD_H C_ATTRIBUTE_FASTCALL
# (no fastcall on x86_64 SysV).

def main(src, dst):
    out = []
    for line in open(src):
        m = re.match(r"^#undef (\w+)\s*$", line)
        if m and m.group(1) in DEFS:
            out.append(f"#define {m.group(1)} {DEFS[m.group(1)]}\n")
        else:
            out.append(line)
    open(dst, "w").write("".join(out))

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
