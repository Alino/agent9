/* mimalloc.h — ladybird9 SHIM, not real mimalloc.
 *
 * Real mimalloc's arena layer reserves address space and commits lazily;
 * cc9's mmap is malloc-backed, so those assumptions are false on 9front.
 * Ladybird's actual mi_* surface is small (AK/kmalloc.cpp is the only user);
 * this header declares exactly that surface plus the no-op option/version
 * calls, all mapped onto cc9 libc malloc in mimalloc-shim.c.
 */
#ifndef MIMALLOC_H
#define MIMALLOC_H

#define MI_MALLOC_VERSION 227 /* impersonates the vcpkg pin 2.2.7 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mi_heap_s mi_heap_t;

void*  mi_malloc(size_t size);
void*  mi_calloc(size_t count, size_t size);
void*  mi_zalloc(size_t size);
void*  mi_realloc(void* p, size_t newsize);
void   mi_free(void* p);
size_t mi_good_size(size_t size);
size_t mi_usable_size(const void* p);

void*  mi_malloc_aligned(size_t size, size_t alignment);
void*  mi_zalloc_aligned(size_t size, size_t alignment);
void*  mi_realloc_aligned(void* p, size_t newsize, size_t alignment);

/* AK partitions heaps; here every heap is the one malloc heap and mi_free
 * works on heap allocations, matching real mimalloc's contract. */
mi_heap_t* mi_heap_new(void);
mi_heap_t* mi_heap_get_default(void);
void*      mi_heap_malloc(mi_heap_t* heap, size_t size);
void*      mi_heap_realloc(mi_heap_t* heap, void* p, size_t newsize);

int mi_version(void);

typedef int mi_option_t; /* accepted and ignored */
void mi_option_set(mi_option_t option, long value);
void mi_option_set_enabled(mi_option_t option, int enable);

#ifdef __cplusplus
}
#endif
#endif /* MIMALLOC_H */
