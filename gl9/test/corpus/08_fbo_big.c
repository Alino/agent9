/* 08_fbo_big — render-to-texture through an FBO, then glReadPixels it back.
 *
 * Every other test in this corpus draws into the DEFAULT framebuffer (OSMesa's
 * client buffer) — which is the one path the 6/6 parity suite ever exercised.
 * Surfman does not do that: its surfaces are a texture with an FBO around it
 * (base/egl/surface.rs::new_generic), WebRender renders into that FBO, and Servo
 * reads the frame back with glReadPixels while it is bound. So render-to-texture
 * is on the critical path for Servo and was completely untested here.
 *
 * Servo on 9front produced a valid 1024x740 PNG in which every pixel was
 * (0,0,0,0) — this test asks, in ~100 lines instead of a 4-minute browser build,
 * whether gl9's FBO path is the reason.
 *
 * Note surfman only checks FBO completeness under debug_assert!, which is
 * compiled out of a release build — so an incomplete FBO there is silent. Here it
 * is a hard failure.
 *
 * Build/run on 9front (cc9), needs GALLIUM_DRIVER + GALLIUM_NOSSE like any gl9 app:
 *   GALLIUM_DRIVER=softpipe GALLIUM_NOSSE=1 08_fbo_big
 * Expects "08_fbo_big N/N PASS".
 */
#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include "gl9egl_platform.h"

#define W 1024
#define H 740

static int pass, total;

static void
ok(const char *what, int cond, const char *detail)
{
	total++;
	printf("%d %s: %s %s\n", total, what, cond ? "PASS" : "FAIL", detail ? detail : "");
	if (cond) pass++;
}

int
main(void)
{
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLContext ctx;
	EGLSurface surf;
	EGLint n;
	static const EGLint cfgattr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
	static const EGLint ctxattr[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
					  EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
	static const EGLint pbattr[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
	GLuint tex, fbo;
	GLenum status;
	static unsigned char px[W * H * 4];
	char d[96];

	dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(dpy, 0, 0)) { printf("eglInitialize failed\n"); return 1; }
	eglBindAPI(EGL_OPENGL_API);
	if (!eglChooseConfig(dpy, cfgattr, &cfg, 1, &n) || n < 1) {
		printf("eglChooseConfig failed\n"); return 1; }
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
	if (ctx == EGL_NO_CONTEXT) { printf("eglCreateContext failed\n"); return 1; }
	/* A pbuffer, as surfman's mesa_surfaceless backend uses — no window. */
	surf = eglCreatePbufferSurface(dpy, cfg, pbattr);
	if (surf == EGL_NO_SURFACE) { printf("eglCreatePbufferSurface failed\n"); return 1; }
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) { printf("eglMakeCurrent failed\n"); return 1; }

	/* Same construction order surfman uses: texture, then FBO around it. */
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	ok("glTexImage2D", glGetError() == GL_NO_ERROR, 0);

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

	/* THE question. surfman only debug_asserts this, so in release an incomplete
	 * FBO silently renders nowhere — which looks exactly like our blank PNG. */
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	snprintf(d, sizeof d, "status=0x%x (want 0x%x COMPLETE)", status, GL_FRAMEBUFFER_COMPLETE);
	ok("FBO complete", status == GL_FRAMEBUFFER_COMPLETE, d);

	/* Clear the FBO to an unmistakable colour and read it straight back. */
	glViewport(0, 0, W, H);
	glClearColor(0.25f, 0.50f, 0.75f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	ok("glClear on FBO", glGetError() == GL_NO_ERROR, 0);

	memset(px, 0xAB, sizeof px);   /* poison: zeros must come from the driver */
	/* gleam (which is how Servo calls this) sets PACK_ALIGNMENT=1 first, and also
	 * reads PACK_ROW_LENGTH. Mirror it exactly — the default of 4 is NOT what
	 * Servo's readback runs with. */
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	{
		GLint rl = -1, pbo = -1;
		glGetIntegerv(GL_PACK_ROW_LENGTH, &rl);
		glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
		printf("   [state] PACK_ROW_LENGTH=%d PIXEL_PACK_BUFFER_BINDING=%d\n", rl, pbo);
	}
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	ok("glReadPixels", glGetError() == GL_NO_ERROR, 0);

	snprintf(d, sizeof d, "got RGBA(%d,%d,%d,%d) want ~(64,128,191,255)",
	         px[0], px[1], px[2], px[3]);
	ok("first pixel has the cleared colour",
	   px[0] > 54 && px[0] < 74 && px[1] > 118 && px[1] < 138 &&
	   px[2] > 181 && px[2] < 201 && px[3] == 255, d);

	/* THE question this test exists for. Servo reads back 1024x740 and gets
	 * exactly ONE correct pixel at (0,0), the rest untouched — so count how many
	 * pixels glReadPixels really filled at this size, not just the first. */
	{
		unsigned long good = 0, i;
		for (i = 0; i < (unsigned long)W * H; i++)
			if (px[i*4] > 54 && px[i*4] < 74 && px[i*4+3] == 255) good++;
		snprintf(d, sizeof d, "%lu of %lu pixels filled", good, (unsigned long)W * H);
		ok("readback filled the WHOLE buffer", good == (unsigned long)W * H, d);
	}

	/* All-zero is the exact symptom Servo showed; call it out by name. */
	{
		int nonzero = 0;
		for (unsigned i = 0; i < sizeof px; i++) if (px[i]) { nonzero = 1; break; }
		ok("readback is not all-zero", nonzero,
		   nonzero ? 0 : "every pixel (0,0,0,0) — same as Servo's blank frame");
	}

	printf("08_fbo_big %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
