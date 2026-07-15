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

/* pull the DRM nr out of a full request code (low 8 bits) */
#define NR(req) ((int)((req) & 0xff))

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
	case I915_PARAM_MMAP_GTT_VERSION:    v = 4; break;
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

	case DRM_I915_GEM_CREATE:
		fprintf(stderr, "[gpu9] GEM_CREATE  <-- iris wants memory (M9: gpu9_alloc)\n");
		errno = ENOSYS; return -1;
	case DRM_I915_GEM_GET_APERTURE:
		fprintf(stderr, "[gpu9] GEM_GET_APERTURE (M9)\n");
		errno = ENOSYS; return -1;
	case DRM_I915_GEM_CONTEXT_CREATE:
		fprintf(stderr, "[gpu9] GEM_CONTEXT_CREATE (M10)\n");
		errno = ENOSYS; return -1;
	case DRM_I915_GEM_CONTEXT_SETPARAM:
		fprintf(stderr, "[gpu9] GEM_CONTEXT_SETPARAM (M10)\n");
		errno = ENOSYS; return -1;
	case DRM_I915_GEM_CONTEXT_GETPARAM:
		fprintf(stderr, "[gpu9] GEM_CONTEXT_GETPARAM (M10)\n");
		errno = ENOSYS; return -1;
	case DRM_I915_GEM_EXECBUFFER2:
		fprintf(stderr, "[gpu9] GEM_EXECBUFFER2  <-- the finish line (M10: the ring)\n");
		errno = ENOSYS; return -1;
	case DRM_I915_GEM_MMAP_GTT:	/* == MMAP_OFFSET's nr */
	case DRM_I915_GEM_MMAP:
		fprintf(stderr, "[gpu9] GEM_MMAP (M9: aperture pointer)\n");
		errno = ENOSYS; return -1;
	case DRM_I915_GEM_WAIT:
		fprintf(stderr, "[gpu9] GEM_WAIT\n");
		errno = ENOSYS; return -1;
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
