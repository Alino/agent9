/*
 * iristri — drive Mesa's iris far enough to see what it asks the "kernel" for.
 *
 * M8: create the screen. iris_screen_create needs a real pipe_screen_config
 * (its very first act is config->options->...), so we build the driconf option
 * caches exactly as the DRI loader does — from iris's own option tables
 * (driinfo_gallium.h common + driinfo_iris.h specific). Then every GEM ioctl
 * iris issues is logged by gpu9/shim/gpu9_ioctl.c, printing iris's real
 * requirements in order.
 */
#include <stdio.h>
#include <fcntl.h>
#include <pthread.h>
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "pipe/p_screen.h"
#include "util/format/u_formats.h"

/* Does thread creation work in THIS binary? iris's screen-create fails at its
 * shader-compiler thread pool (util_queue_init -> thrd_create), so isolate
 * whether cc9 threads work here at all before blaming iris. */
static void *thr_probe(void *a){ *(int*)a = 42; return (void*)0; }
static void
test_threads(void)
{
	pthread_t t;
	int r, v = 0;
	r = pthread_create(&t, (void*)0, thr_probe, &v);
	if(r != 0){ fprintf(stderr, "iristri: pthread_create FAILED (%d)\n", r); return; }
	pthread_join(t, (void*)0);
	fprintf(stderr, "iristri: pthread OK (v=%d)\n", v);
}

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#endif

/* the full iris option set — the loader merges these two the same way */
static const driOptionDescription iris_driconf[] = {
#include "driinfo_gallium.h"
#include "driinfo_iris.h"
};

extern int gpu9dev_open(void);
struct pipe_screen *iris_screen_create(int fd, const struct pipe_screen_config *config);

int
main(void)
{
	static driOptionCache options, options_info;
	struct pipe_screen_config config;
	struct pipe_screen *screen;
	int fd;

	setvbuf(stderr, (void*)0, _IONBF, 0);	/* unbuffered: keep marker order true */

	/* iris fstat()s the fd first thing (to key the bufmgr), so it must be a
	 * real descriptor. The value is otherwise opaque to gpu9 — our ioctl()
	 * dispatches on the request code, not the fd. */
	fd = open("/dev/null", O_RDWR);
	if(fd < 0){ fprintf(stderr, "iristri: cannot open /dev/null\n"); return 2; }

	fprintf(stderr, "iristri: threads BEFORE gpu open:\n");
	test_threads();
	if(gpu9dev_open() == 0){
		fprintf(stderr, "iristri: threads AFTER gpu segattach:\n");
		test_threads();
	}

	driParseOptionInfo(&options_info, iris_driconf, ARRAY_SIZE(iris_driconf));
	driParseConfigFiles(&options, &options_info, 0, "iris",
		"i915", NULL, "iristri", 0, NULL, 0);
	config.options = &options;
	config.options_info = &options_info;

	fprintf(stderr, "iristri: iris_screen_create(fd=%d, config)\n", fd);
	screen = iris_screen_create(fd, &config);
	fprintf(stderr, "iristri: iris_screen_create -> %p (%s)\n",
		(void*)screen, screen ? "SCREEN CREATED" : "NULL");
	if(!screen) return 1;
	fprintf(stderr, "iristri: ENTER RENDER BLOCK\n"); fflush(stderr);

	/* M10/M11: create a context and clear a render target to green. This makes
	 * iris compile a BLORP clear shader and submit a real 3D batch via
	 * EXECBUFFER2 -> gpu9dev_bind + gpu9dev_exec. Then read the pixels back. */
	{
		struct pipe_screen *ps = screen;
		struct pipe_context *ctx;
		struct pipe_resource tmpl, *rt;
		struct pipe_surface stmpl, *surf;
		union pipe_color_union col;
		struct pipe_transfer *xfer = (void*)0;
		unsigned *px;
		int i, green = 0, total;

		fprintf(stderr, "iristri: calling context_create...\n"); fflush(stderr);
		ctx = ps->context_create(ps, (void*)0, 0);
		if(!ctx){ fprintf(stderr, "iristri: context_create FAILED\n"); return 1; }

		memset(&tmpl, 0, sizeof tmpl);
		tmpl.target = PIPE_TEXTURE_2D;
		tmpl.format = PIPE_FORMAT_B8G8R8A8_UNORM;
		tmpl.width0 = 64; tmpl.height0 = 64; tmpl.depth0 = 1; tmpl.array_size = 1;
		tmpl.bind = PIPE_BIND_RENDER_TARGET;
		fprintf(stderr, "iristri: resource_create 64x64 rt\n");
		rt = ps->resource_create(ps, &tmpl);
		if(!rt){ fprintf(stderr, "iristri: resource_create FAILED\n"); return 1; }

		memset(&stmpl, 0, sizeof stmpl);
		stmpl.format = tmpl.format;
		surf = ctx->create_surface(ctx, rt, &stmpl);
		if(!surf){ fprintf(stderr, "iristri: create_surface FAILED\n"); return 1; }

		col.f[0] = 0.0f; col.f[1] = 1.0f; col.f[2] = 0.0f; col.f[3] = 1.0f;	/* green */
		fprintf(stderr, "iristri: clear_render_target -> SUBMIT (GPU renders now)\n");
		ctx->clear_render_target(ctx, surf, &col, 0, 0, 64, 64, false);
		ctx->flush(ctx, (void*)0, 0);
		fprintf(stderr, "iristri: flush returned (batch completed)\n");

		/* read the pixels back and verify green (BGRA -> low 24 bits 0x00ff00) */
		{
			struct pipe_box box; memset(&box, 0, sizeof box);
			box.width = 64; box.height = 64; box.depth = 1;
			px = (unsigned*)ctx->texture_map(ctx, rt, 0, PIPE_MAP_READ, &box, &xfer);
		}
		if(px){
			total = 64*64;
			for(i = 0; i < total; i++) if((px[i] & 0x00ffffff) == 0x0000ff00) green++;
			fprintf(stderr, "iristri: %d/%d pixels are GREEN (0x%08x sample)\n", green, total, px[0]);
			ctx->texture_unmap(ctx, xfer);
			fprintf(stderr, green > total/2 ? "iristri: *** GPU RENDER VERIFIED ***\n"
			                                : "iristri: render did not produce green\n");
		} else fprintf(stderr, "iristri: texture_map returned NULL\n");
	}
	return 0;
}
