#ifndef _MALLOC_H
#define _MALLOC_H
#include <stdlib.h>
/* glibc mallinfo/mallinfo2 — LLVM's Support/Unix/Process.inc GetMallocUsage()
 * uses them (config.h defines HAVE_MALLINFO2 from the linux configure host).
 * Informational only (heap-usage reporting), never on the JIT path — return
 * zeros. Fields match glibc so member accesses compile. */
struct mallinfo  { int    arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
struct mallinfo2 { size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
#ifdef __cplusplus
extern "C" {
#endif
struct mallinfo  mallinfo(void);
struct mallinfo2 mallinfo2(void);
#ifdef __cplusplus
}
#endif
#endif
