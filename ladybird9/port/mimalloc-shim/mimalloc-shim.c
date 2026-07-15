/* mimalloc-shim.c — the mi_* surface Ladybird uses (AK/kmalloc.cpp), mapped
 * onto cc9 libc malloc. See mimalloc.h for why real mimalloc is not portable
 * to cc9's malloc-backed mmap.
 *
 * ponytail: plain-malloc shim; revisit iff cc9 ever grows real mmap and the
 * partition heaps start mattering for performance.
 */
#include "mimalloc.h"

#include <malloc.h> /* malloc_usable_size */
#include <stdlib.h>
#include <string.h>

void* mi_malloc(size_t n) { return malloc(n); }
void* mi_calloc(size_t c, size_t n) { return calloc(c, n); }
void* mi_zalloc(size_t n) { return calloc(1, n); }
void* mi_realloc(void* p, size_t n) { return realloc(p, n); }
void  mi_free(void* p) { free(p); }

size_t mi_good_size(size_t n) { return n; }
size_t mi_usable_size(const void* p) { return p ? malloc_usable_size((void*)p) : 0; }

static void* alloc_aligned(size_t n, size_t align, int zero)
{
	void* p = NULL;
	if (align < sizeof(void*))
		align = sizeof(void*); /* posix_memalign precondition */
	if (posix_memalign(&p, align, n ? n : 1) != 0)
		return NULL;
	if (zero)
		memset(p, 0, n);
	return p;
}

void* mi_malloc_aligned(size_t n, size_t align) { return alloc_aligned(n, align, 0); }
void* mi_zalloc_aligned(size_t n, size_t align) { return alloc_aligned(n, align, 1); }

void* mi_realloc_aligned(void* p, size_t n, size_t align)
{
	if (align <= 16)
		return realloc(p, n); /* malloc alignment already suffices */
	void* q = alloc_aligned(n, align, 0);
	if (!q)
		return NULL; /* mi contract: p stays valid on failure */
	if (p) {
		size_t old = malloc_usable_size(p); /* >= requested old size */
		memcpy(q, p, old < n ? old : n);
		free(p);
	}
	return q;
}

/* every partition heap is the malloc heap */
struct mi_heap_s { int unused; };
static struct mi_heap_s the_heap;

mi_heap_t* mi_heap_new(void) { return &the_heap; }
mi_heap_t* mi_heap_get_default(void) { return &the_heap; }
void* mi_heap_malloc(mi_heap_t* h, size_t n) { (void)h; return malloc(n); }
void* mi_heap_realloc(mi_heap_t* h, void* p, size_t n) { (void)h; return realloc(p, n); }

int  mi_version(void) { return MI_MALLOC_VERSION; }
void mi_option_set(mi_option_t o, long v) { (void)o; (void)v; }
void mi_option_set_enabled(mi_option_t o, int e) { (void)o; (void)e; }
