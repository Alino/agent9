/* zig9: glue between the CBE-emitted zig and cc9's runtime.
 * The CBE mangles extern names that start with `__` (reserved in C), so Zig
 * code can't reference __cc9_tos directly — expose it under a clean name. */
extern void *__cc9_tos; /* set by cc9 crt0._start from entry rax */

void *zig9_tos(void) { return __cc9_tos; }
