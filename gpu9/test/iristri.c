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
struct pipe_screen;
struct pipe_screen_config { struct driOptionCache *options;
                            const struct driOptionCache *options_info; };
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
		(void*)screen, screen ? "SCREEN CREATED" : "NULL - see the last ioctl above");
	return screen ? 0 : 1;
}
