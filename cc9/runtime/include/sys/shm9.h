#ifndef _SYS_SHM9_H
#define _SYS_SHM9_H
/* shm9 — cross-process anonymous shared memory over 9front named global
 * segments (segment(3), the #g kernel device).
 *
 * Model: cc9_shm_create() makes a named segment and returns an fd onto its
 * #g/<name>/data file. mmap(PROT_READ|PROT_WRITE, MAP_SHARED, fd) on that fd
 * segattaches the segment — at the SAME virtual address in every process (a
 * devsegment property; the VA is fixed at creation). The fd is the handle:
 * it survives dup() and can be re-derived from a name, so a buffer crosses an
 * IPC boundary as the wire triple {name, offset, len} (offset is 0 in Phase A;
 * it is on the wire from day one so the Phase B pool allocator changes no
 * protocol).
 *
 * Lifetime: the kernel refcounts attachers, but the NAME (the #g dir) is what
 * keeps a segment attachable and its contents alive. Nobody removes eagerly —
 * multi-hop forwarding (ImageDecoder -> WebContent -> Compositor) means the
 * creator can't know when the last consumer arrives. cc9_shm_sweep() garbage-
 * collects: a name whose segment is attached in NO process for longer than the
 * grace window is removed. Run it periodically from a long-lived process (the
 * browser UI) and once at startup (cleans up after crashes). */
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Longest segment name we generate ("shm.<pid>.<seq>"), incl. NUL. 9front's
 * KNAMELEN is 28; stay under it. */
#define CC9_SHM_NAMELEN 28

/* Create an anonymous shared segment of (page-rounded) size bytes.
 * Returns an fd onto #g/<name>/data (FD_CLOEXEC set), or -1 + errno.
 * The fd is mmap(MAP_SHARED)-able and close()d like any fd. */
int cc9_shm_create(unsigned long size);

/* Derive the wire handle for a shm fd: name (>= CC9_SHM_NAMELEN bytes),
 * offset and len of the buffer within the segment. Phase A: offset 0,
 * len = segment length. Returns 0, or -1 + errno if fd is not a shm fd. */
int cc9_shm_export(int fd, char *name, unsigned long *offset, unsigned long *len);

/* Open an existing named segment received over IPC. Returns an fd
 * (FD_CLOEXEC set) usable with mmap(MAP_SHARED), or -1 + errno. */
int cc9_shm_import(const char *name, unsigned long offset, unsigned long len);

/* Remove segment names matching prefix whose segments are attached in no
 * process and have stayed that way for at least grace_seconds. Also removes
 * names whose creator pid is dead AND that are attached nowhere (crash
 * cleanup, grace still applies from first observation). Safe to call from
 * any process; cheap when nothing is stale. */
void cc9_shm_sweep(const char *prefix, int grace_seconds);

/* Startup crash-cleanup: immediately remove pools "<prefix><pid>.<seq>" whose
 * owner pid is dead (no /proc/<pid>) and which are attached in no live process.
 * No grace window — a dead owner has no pending attach. Call once at process
 * start so a killed/crashed run's leaked 256 MiB pool doesn't survive to
 * exhaust the ~100-entry #g cap. Returns the count reaped. */
int cc9_shm_reap_dead(const char *prefix);

/* Internal hooks for posix_llvm.c's mmap/munmap routing — not for callers.
 * cc9_shm_try_map: if fd is a #g data fd, attach and return the VA (or
 * MAP_FAILED on real failure, setting *handled=1); else *handled=0.
 * cc9_shm_unmap: returns 1 if p was a shm mapping (detached), else 0. */
void *cc9_shm_try_map(int fd, unsigned long len, int *handled);
int   cc9_shm_unmap(void *p, unsigned long len);

#ifdef __cplusplus
}
#endif
#endif
