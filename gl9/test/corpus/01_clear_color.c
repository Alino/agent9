/* 01_clear_color — glClearColor + glClear. Proves context + buffer readback. */
#include "glharness.h"

static void
gl9_render(void)
{
	glClearColor(0.2f, 0.4f, 0.8f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
}
