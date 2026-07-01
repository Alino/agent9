/* 00_clear_probe — the smallest proof softpipe runs on 9front: make an OSMesa
 * context on a 16x16 RGBA buffer, clear it red, read pixel (0,0), print it.
 * Success == "R=255 G=0 B=0". No PPM, no draw, no parity harness yet. */
#include <stdio.h>
#include <unistd.h>
#include <GL/osmesa.h>
#include <GL/gl.h>

/* raw trace to fd 2 — reliable even if buffered stdio or a fault is in play */
static void trace(const char *s){ write(2, s, (int)__builtin_strlen(s)); }

int
main(void)
{
	unsigned char buf[16 * 16 * 4];
	OSMesaContext ctx;

	trace("START\n");
	ctx = OSMesaCreateContext(OSMESA_RGBA, 0);
	trace("after OSMesaCreateContext\n");
	if (!ctx) {
		printf("OSMesaCreateContext failed\n");
		return 1;
	}
	if (!OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, 16, 16)) {
		printf("OSMesaMakeCurrent failed\n");
		return 1;
	}
	trace("after OSMesaMakeCurrent\n");
	printf("GL_VERSION=%s\n", (const char *)glGetString(GL_VERSION));
	glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	printf("R=%d G=%d B=%d A=%d\n", buf[0], buf[1], buf[2], buf[3]);
	OSMesaDestroyContext(ctx);
	return (buf[0] == 255 && buf[1] == 0 && buf[2] == 0) ? 0 : 2;
}
