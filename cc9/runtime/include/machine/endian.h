/* BSD-style machine/endian.h for the cc9 freestanding target (x86_64, LE).
 * LLVM's ADT/bit.h falls back to this when no OS macro matches. */
#ifndef CC9_MACHINE_ENDIAN_H
#define CC9_MACHINE_ENDIAN_H
#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN    4321
#define PDP_ENDIAN    3412
#define BYTE_ORDER    LITTLE_ENDIAN
#endif
