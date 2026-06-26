#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H
#include <sys/types.h>
/* cc9 has no real mmap. mmap() is a read-fallback shim (malloc + pread the file
 * region) so LLVM's MemoryBuffer file path works; anonymous mappings are
 * malloc. munmap frees. mprotect is a no-op (W^X is the kernel's call). There is
 * no shared/file-backed coherency — read-only consumers (the compiler) are fine. */
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED  1
#define MAP_PRIVATE 2
#define MAP_FIXED   0x10
#define MAP_ANON    0x20
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)
#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4
#define MADV_NORMAL    0
#define MADV_WILLNEED  3
#define MADV_DONTNEED  4
#ifdef __cplusplus
extern "C" {
#endif
void *mmap(void *, size_t, int, int, int, off_t);
int   munmap(void *, size_t);
int   mprotect(void *, size_t, int);
int   madvise(void *, size_t, int);
int   msync(void *, size_t, int);
#ifdef __cplusplus
}
#endif
#endif
