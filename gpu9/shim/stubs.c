/*
 * stubs.c — symbols iris references but that no render path reaches on 9front.
 *
 * Two families, both excluded from the build on purpose:
 *
 *  xe_*  : the Xe kernel-driver backend (Tiger Lake+). Broadwell is i915, so the
 *          xe TUs are dead weight that only fail on Xe uapi we will never have.
 *          intel_kmd.c dispatches on the kmd type, so it still REFERENCES them.
 *          xe_get_backend() returning NULL makes that dispatch take the i915 path
 *          — which is the one gpu9 implements.
 *
 *  intel_spec_* / intel_group_* / intel_field_iterator_* : the batch DECODER,
 *          a debug aid (INTEL_DEBUG=bat) that pretty-prints command streams from
 *          an XML spec. It wants expat. iris only calls it when asked to dump a
 *          batch, so returning "no spec" disables the pretty-printer and changes
 *          nothing about rendering.
 *
 * Every stub fails loudly-but-safely (NULL / 0 / false) so callers take their
 * error branch instead of proceeding on a lie.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- Xe backend: never used on Gen8 ---- */
void *xe_get_backend(void) { return NULL; }
bool xe_gem_read_render_timestamp(int fd, uint64_t *value)
	{ (void)fd; (void)value; return false; }
bool xe_gem_read_correlate_cpu_gpu_timestamp(int fd, int engine_class,
	uint16_t engine_instance, int clock_id, uint64_t *cpu_ts,
	uint64_t *gpu_ts, uint64_t *delta)
	{ (void)fd; (void)engine_class; (void)engine_instance; (void)clock_id;
	  (void)cpu_ts; (void)gpu_ts; (void)delta; return false; }
bool xe_gem_can_render_on_fd(int fd) { (void)fd; return false; }
void *xe_engine_get_info(int fd) { (void)fd; return NULL; }
bool intel_device_info_xe_get_info_from_fd(int fd, void *devinfo)
	{ (void)fd; (void)devinfo; return false; }
bool intel_device_info_xe_query_hwconfig(int fd, void *devinfo, void *out)
	{ (void)fd; (void)devinfo; (void)out; return false; }
bool intel_device_info_xe_query_regions(int fd, void *devinfo, bool update)
	{ (void)fd; (void)devinfo; (void)update; return false; }

/* iris's own xe halves, reached only via `switch (devinfo->kmd_type) case
 * INTEL_KMD_TYPE_XE` in iris_batch.c. Broadwell reports INTEL_KMD_TYPE_I915,
 * so these are dead branches — but the switch still needs them to link. */
struct iris_context; struct iris_batch; struct iris_bufmgr;
void iris_xe_init_batches(struct iris_context *ice) { (void)ice; }
void iris_xe_destroy_batch(struct iris_batch *batch) { (void)batch; }
bool iris_xe_replace_batch(struct iris_batch *batch) { (void)batch; return false; }
bool iris_xe_init_global_vm(struct iris_bufmgr *bufmgr, uint32_t *vm_id)
	{ (void)bufmgr; (void)vm_id; return false; }
void iris_xe_destroy_global_vm(struct iris_bufmgr *bufmgr) { (void)bufmgr; }

/* ---- batch decoder: debug pretty-printer, hard-wants expat ---- */
void *intel_spec_load(const void *devinfo) { (void)devinfo; return NULL; }
void *intel_spec_load_from_path(const void *devinfo, const char *path)
	{ (void)devinfo; (void)path; return NULL; }
void  intel_spec_destroy(void *spec) { (void)spec; }
uint32_t intel_spec_get_gen(void *spec) { (void)spec; return 0; }
void *intel_spec_find_instruction(void *spec, const void *engine, const uint32_t *p)
	{ (void)spec; (void)engine; (void)p; return NULL; }
void *intel_spec_find_register(void *spec, uint32_t offset)
	{ (void)spec; (void)offset; return NULL; }
void *intel_spec_find_struct(void *spec, const char *name)
	{ (void)spec; (void)name; return NULL; }
int   intel_group_get_length(void *group, const uint32_t *p)
	{ (void)group; (void)p; return -1; }
const char *intel_group_get_name(void *group) { (void)group; return "?"; }
void  intel_field_iterator_init(void *iter, void *group, const uint32_t *p,
	uint32_t offset, bool print_colours)
	{ (void)iter; (void)group; (void)p; (void)offset; (void)print_colours; }
bool  intel_field_iterator_next(void *iter) { (void)iter; return false; }
void  intel_print_group(void *out, void *group, uint64_t offset,
	const uint32_t *p, uint32_t p_bit, bool colour)
	{ (void)out; (void)group; (void)offset; (void)p; (void)p_bit; (void)colour; }
