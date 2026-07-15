/*
 * gpu9_ioctl.c — the "kernel driver", in-process.
 *
 * iris reaches the kernel through exactly one door: intel_gem.h's intel_ioctl()
 * -> ioctl(fd, DRM_IOCTL_I915_*, arg). cc9's ioctl() routes every 'd'-type
 * (DRM) request here (see cc9/runtime/fs.c). So this file IS iris's kernel.
 *
 * M8 goal is not to render — it is to LET IRIS TELL US WHAT IT NEEDS. Every
 * ioctl is logged with its decoded name; the init queries (GETPARAM, QUERY,
 * CONTEXT params) return the values Broadwell's i915 would, so iris walks all
 * the way through screen + context setup and we see the exact GEM sequence it
 * would issue for a triangle. GEM_CREATE/EXECBUFFER2/etc. are logged and
 * stubbed here; wiring them to gpu9's ring is M9/M10.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include "drm-uapi/i915_drm.h"
#include "gpu9.h"

/* pull the DRM nr out of a full request code (low 8 bits) */
#define NR(req) ((int)((req) & 0xff))

/* the gpu9 device backend (gpu9_dev.c) */
extern int      gpu9dev_open(void);
extern uint32_t gpu9dev_alloc(uint32_t bytes, void **cpu);
extern void    *gpu9dev_cpu(uint32_t ggtt);
extern uint64_t gpu9dev_aperture_free(void);
extern uint64_t gpu9dev_aperture_total(void);
extern int      gpu9dev_exec(uint32_t batch_ggtt);
extern void     gpu9dev_bind(uint32_t va, uint32_t ggtt, uint32_t bytes);

/* BO handle table. iris makes a BO (GEM_CREATE -> handle), maps it (GEM_MMAP),
 * assigns it a softpin GPU address in userspace, then references it by that
 * address in EXECBUFFER2. We remember the GGTT offset (== aperture offset) of
 * each handle's backing so we can alias the softpin address to it at exec. */
#define MAXBO 4096
struct bo { uint32_t ggtt; uint32_t size; void *cpu; int used; };
static struct bo botab[MAXBO];
static int nbo = 1;	/* handle 0 is "none" */

static int
lazy_open(void)
{
	static int tried, ok;
	if(!tried){ tried = 1; ok = gpu9dev_open() == 0; }
	if(!ok){ errno = ENODEV; return -1; }
	return 0;
}

static const char *
paramname(int p)
{
	switch(p){
	case I915_PARAM_CHIPSET_ID: return "CHIPSET_ID";
	case I915_PARAM_REVISION: return "REVISION";
	case I915_PARAM_HAS_EXEC_SOFTPIN: return "HAS_EXEC_SOFTPIN";
	case I915_PARAM_HAS_ALIASING_PPGTT: return "HAS_ALIASING_PPGTT";
	case I915_PARAM_HAS_EXEC_NO_RELOC: return "HAS_EXEC_NO_RELOC";
	case I915_PARAM_HAS_EXEC_FENCE_ARRAY: return "HAS_EXEC_FENCE_ARRAY";
	case I915_PARAM_HAS_EXEC_ASYNC: return "HAS_EXEC_ASYNC";
	case I915_PARAM_HAS_EXEC_CAPTURE: return "HAS_EXEC_CAPTURE";
	case I915_PARAM_HAS_EXEC_BATCH_FIRST: return "HAS_EXEC_BATCH_FIRST";
	case I915_PARAM_HAS_CONTEXT_ISOLATION: return "HAS_CONTEXT_ISOLATION";
	case I915_PARAM_HAS_SCHEDULER: return "HAS_SCHEDULER";
	case I915_PARAM_CS_TIMESTAMP_FREQUENCY: return "CS_TIMESTAMP_FREQUENCY";
	case I915_PARAM_MMAP_GTT_VERSION: return "MMAP_GTT_VERSION";
	case I915_PARAM_NUM_FENCES_AVAIL: return "NUM_FENCES_AVAIL";
	default: return "?";
	}
}

/* the P-state / device values gpu9 already knows for cirno's Broadwell GT1 */
static int
getparam(struct drm_i915_getparam *gp)
{
	int v;

	switch(gp->param){
	case I915_PARAM_CHIPSET_ID:          v = 0x1606; break;
	case I915_PARAM_REVISION:            v = 0x09;   break;
	case I915_PARAM_HAS_EXEC_SOFTPIN:    v = 1; break;	/* iris REQUIRES this */
	case I915_PARAM_HAS_ALIASING_PPGTT:  v = 2; break;	/* 2 = full ppgtt (per-ctx) */
	case I915_PARAM_HAS_EXEC_NO_RELOC:   v = 1; break;
	case I915_PARAM_HAS_EXEC_FENCE_ARRAY:v = 1; break;
	case I915_PARAM_HAS_EXEC_ASYNC:      v = 1; break;
	case I915_PARAM_HAS_EXEC_CAPTURE:    v = 1; break;
	case I915_PARAM_HAS_EXEC_BATCH_FIRST:v = 1; break;
	case I915_PARAM_HAS_CONTEXT_ISOLATION: v = 1; break;
	case I915_PARAM_HAS_SCHEDULER:       v = 0; break;	/* no preemption/priority */
	case I915_PARAM_CS_TIMESTAMP_FREQUENCY: v = 12000000; break;	/* BDW: 12MHz */
	/* MMAP_GTT_VERSION intentionally UNHANDLED (-> -EINVAL): forces iris down the
	 * old GEM_MMAP path, which returns a CPU pointer directly. gpu9's BOs are
	 * already aperture-mapped, so we hand back aper+ggtt; the MMAP_OFFSET+mmap
	 * path would need cc9's mmap to understand GPU tokens, which it doesn't. */
	case I915_PARAM_NUM_FENCES_AVAIL:    v = 32; break;
	default:
		/* Real i915 returns -EINVAL for unknown params, and iris overwrites a
		 * devinfo field ONLY on getparam success. So FAIL here — returning
		 * "success, value 0" would clobber iris's static gen8 topology with
		 * zeros and divide-by-zero in update_from_masks. This was the bug. */
		fprintf(stderr, "[gpu9] GETPARAM %d (%s) -> EINVAL (keep static)\n",
			gp->param, paramname(gp->param));
		errno = EINVAL;
		return -1;
	}
	fprintf(stderr, "[gpu9] GETPARAM %d (%s) -> %d\n", gp->param, paramname(gp->param), v);
	if(gp->value)
		*gp->value = v;
	return 0;
}

/* core (non-i915) DRM ioctls: nr < DRM_COMMAND_BASE */
static int
core_ioctl(int cmd, void *arg)
{
	switch(cmd){
	case 0x09:	/* DRM_IOCTL_GEM_CLOSE */
		return 0;
	default:
		fprintf(stderr, "[gpu9] core DRM ioctl 0x%02x -> UNHANDLED\n", cmd);
		errno = EINVAL; return -1;
	}
}

/*
 * EXECBUFFER2 — the finish line. iris hands us an array of exec_object2 (each a
 * BO handle + the softpin GPU address it chose) and a batch. For GGTT-only
 * submission we: bind every BO's softpin address to its backing pages (so the
 * absolute addresses embedded in the batch resolve), then run the batch at its
 * address on the render ring.
 *
 * REQUIRES iris's softpin addresses to fit our 64MB GGTT — that is the memzone
 * compression (M9b). Until then this logs the addresses (revealing the range to
 * compress to) and returns success without submitting.
 */
static int
gpu9_execbuffer2(struct drm_i915_gem_execbuffer2 *eb)
{
	struct drm_i915_gem_exec_object2 *objs =
		(struct drm_i915_gem_exec_object2*)(uintptr_t)eb->buffers_ptr;
	uint32_t i, bi;
	uint64_t maxva = 0;

	if(lazy_open() < 0) return -1;
	bi = (eb->flags & I915_EXEC_BATCH_FIRST) ? 0 : eb->buffer_count - 1;

	for(i = 0; i < eb->buffer_count; i++){
		uint64_t va = objs[i].offset;
		uint32_t h = objs[i].handle;
		uint32_t sz = (h < (uint32_t)nbo && botab[h].used) ? botab[h].size : 0;
		if(va + sz > maxva) maxva = va + sz;
		fprintf(stderr, "[gpu9] exec obj[%u] handle=%u va=%#llx size=%u%s\n",
			i, h, (unsigned long long)va, sz, i==bi ? " (BATCH)" : "");
	}
	fprintf(stderr, "[gpu9] EXECBUFFER2: %u objs, batch@obj%u +%#x, max VA %#llx (%llu MB)\n",
		eb->buffer_count, bi, eb->batch_start_offset,
		(unsigned long long)maxva, (unsigned long long)(maxva>>20));

	if(maxva > GPU9_APERTURE_SZ){
		fprintf(stderr, "[gpu9] *** softpin VAs exceed 64MB GGTT — need memzone compression (M9b)\n");
		return 0;	/* pretend success so iris proceeds; nothing rendered yet */
	}

	/* addresses fit: bind each BO and run the batch for real */
	for(i = 0; i < eb->buffer_count; i++){
		uint32_t h = objs[i].handle;
		if(h < (uint32_t)nbo && botab[h].used)
			gpu9dev_bind((uint32_t)objs[i].offset, botab[h].ggtt, botab[h].size);
	}
	{
		uint32_t batch_va = (uint32_t)objs[bi].offset + eb->batch_start_offset;
		fprintf(stderr, "[gpu9] submitting batch @ GGTT %#x\n", batch_va);
		return gpu9dev_exec(batch_va);
	}
}

int
gpu9_ioctl(int fd, unsigned long req, void *arg)
{
	int nr = NR(req);
	int cmd;
	(void)fd;

	/* the nr byte is DRM_COMMAND_BASE(0x40) + the i915 command; below that it
	 * is a core DRM ioctl. THIS is the decode M8's first run got wrong. */
	if(nr < DRM_COMMAND_BASE)
		return core_ioctl(nr, arg);
	cmd = nr - DRM_COMMAND_BASE;

	switch(cmd){
	case DRM_I915_GETPARAM:
		return getparam((struct drm_i915_getparam*)arg);

	case DRM_I915_QUERY:
		/* topology/engine query. iris TOLERATES failure and falls back to the
		 * static gen8 devinfo (that is the "Kernel 4.1 required" warning), so
		 * leave it failing for now — M8 only needs the sequence. */
		fprintf(stderr, "[gpu9] QUERY -> EINVAL (iris uses static gen8 info)\n");
		errno = EINVAL; return -1;

	case DRM_I915_GEM_CREATE: {
		struct drm_i915_gem_create *c = arg;
		void *cpu;
		uint32_t ggtt, h;
		if(lazy_open() < 0) return -1;
		if(nbo >= MAXBO){ errno = ENOMEM; return -1; }
		ggtt = gpu9dev_alloc((uint32_t)c->size, &cpu);
		if(ggtt == 0){ errno = ENOMEM; return -1; }
		h = nbo++;
		botab[h].ggtt = ggtt; botab[h].size = (uint32_t)c->size;
		botab[h].cpu = cpu; botab[h].used = 1;
		c->handle = h;
		fprintf(stderr, "[gpu9] GEM_CREATE %llu bytes -> handle %u @ GGTT %#x\n",
			(unsigned long long)c->size, h, ggtt);
		return 0;
	}
	case DRM_I915_GEM_GET_APERTURE: {
		struct drm_i915_gem_get_aperture *a = arg;
		if(lazy_open() < 0) return -1;
		a->aper_size = gpu9dev_aperture_total();
		a->aper_available_size = gpu9dev_aperture_free();
		return 0;
	}
	case DRM_I915_GEM_MMAP: {
		struct drm_i915_gem_mmap *m = arg;
		if(m->handle == 0 || m->handle >= (uint32_t)nbo || !botab[m->handle].used){
			errno = EINVAL; return -1;
		}
		/* our BOs are already CPU-mapped through the aperture — hand the pointer
		 * back directly (offset into the BO honored). */
		m->addr_ptr = (uint64_t)(uintptr_t)botab[m->handle].cpu + m->offset;
		fprintf(stderr, "[gpu9] GEM_MMAP handle %u -> %p\n",
			m->handle, (void*)(uintptr_t)m->addr_ptr);
		return 0;
	}
	case DRM_I915_GEM_MMAP_GTT: {	/* == MMAP_OFFSET's nr */
		/* encode the BO's GGTT offset as the mmap 'offset' token; cc9's mmap
		 * turns it back into the aperture pointer (see the mmap hook). */
		struct drm_i915_gem_mmap_offset *mo = arg;
		if(mo->handle == 0 || mo->handle >= (uint32_t)nbo || !botab[mo->handle].used){
			errno = EINVAL; return -1;
		}
		mo->offset = botab[mo->handle].ggtt;
		fprintf(stderr, "[gpu9] GEM_MMAP_OFFSET handle %u -> token %#llx\n",
			mo->handle, (unsigned long long)mo->offset);
		return 0;
	}
	case DRM_I915_GEM_CONTEXT_CREATE: {
		struct drm_i915_gem_context_create *cc = arg;
		cc->ctx_id = 1;	/* one context; gpu9 has a single ring */
		fprintf(stderr, "[gpu9] GEM_CONTEXT_CREATE -> ctx 1\n");
		return 0;
	}
	case DRM_I915_GEM_CONTEXT_DESTROY:
		return 0;
	case DRM_I915_GEM_CONTEXT_SETPARAM:
		return 0;	/* accept scheduler/vm params silently */
	case DRM_I915_GEM_CONTEXT_GETPARAM: {
		struct drm_i915_gem_context_param *p = arg;
		p->value = 0;
		return 0;
	}
	case DRM_I915_GEM_EXECBUFFER2:
		return gpu9_execbuffer2(arg);
	case DRM_I915_GEM_WAIT:
		return 0;	/* gpu9dev_exec is synchronous — nothing to wait for */
	case DRM_I915_GEM_SET_DOMAIN:
	case DRM_I915_GEM_SET_TILING:
	case DRM_I915_GEM_GET_TILING:
	case DRM_I915_GEM_SET_CACHING:
	case DRM_I915_GEM_MADVISE:
	case DRM_I915_GEM_BUSY:
		fprintf(stderr, "[gpu9] GEM misc cmd=0x%02x -> ok(0)\n", cmd);
		return 0;	/* harmless: pretend it worked */

	default:
		fprintf(stderr, "[gpu9] i915 cmd 0x%02x (req %#lx) -> UNHANDLED\n", cmd, req);
		errno = EINVAL; return -1;
	}
}
