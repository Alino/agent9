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
#define MADV_RANDOM    1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED  3
#define MADV_DONTNEED  4
#ifdef __cplusplus
extern "C" {
#endif
void *mmap(void *, size_t, int, int, int, off_t);
int   munmap(void *, size_t);
int   mprotect(void *, size_t, int);
/* POSIX shared memory: declared so LLVM's Orc MemoryMapper.cpp compiles. Only
 * its cross-process SharedMemoryMapper calls these; the in-process mapper (what
 * an on-box JIT uses) never does. Stubbed to failure in posix_llvm.c. */
int   shm_open(const char *, int, mode_t);
int   shm_unlink(const char *);
int   madvise(void *, size_t, int);
int   msync(void *, size_t, int);
/* Nonzero iff this kernel will hand out writable+executable memory: it carries
 * the wxallow patch AND plan9.ini set wxallow=1. An anonymous PROT_EXEC mmap
 * comes from segattach(SG_EXEC) and only executes when this is true, so a JIT
 * should ask FIRST and fall back to an interpreter otherwise — that is what lets
 * one binary run on both a patched and a stock kernel.
 *
 * Reads the /env gate rather than executing a probe: with wxallow=0 the kernel
 * silently strips SG_EXEC and segattach still succeeds, so an execute-probe's
 * "no" is a fault, not a return value. See posix_llvm.c for the one case this
 * misreads (wxallow=1 asserted on an unpatched kernel). */
int   cc9_have_wx(void);
#ifdef __cplusplus
}
#endif
#endif
