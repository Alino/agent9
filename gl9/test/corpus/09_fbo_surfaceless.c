/* 09_fbo_surfaceless — the FBO-readback path with NO draw surface at all.
 *
 * 07_fbo_readback binds a pbuffer before rendering into its FBO. Ladybird's
 * WebGL CPU-painting path (Services/Compositor/OpenGLContext.cpp) does NOT: it
 * calls eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) and renders
 * entirely into an application FBO (EGL_KHR_surfaceless_context). OSMesa needs
 * *some* buffer to make a context current, so gl9egl binds a private throwaway
 * for the surfaceless case — this test is the isolated proof that it works
 * (~100 lines vs a 300 MB browser ship).
 *
 * Build/run on 9front (cc9):
 *   GALLIUM_DRIVER=softpipe GALLIUM_NOSSE=1 09_fbo_surfaceless
 * Expects "09_fbo_surfaceless N/N PASS".
 */
#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include "gl9egl_platform.h"

#define W 64
#define H 64

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
	EGLint n;
	static const EGLint cfgattr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
	static const EGLint ctxattr[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
					  EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
	GLuint tex, fbo;
	GLenum status;
	unsigned char px[W * H * 4];
	char d[96];

	dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(dpy, 0, 0)) { printf("eglInitialize failed\n"); return 1; }
	eglBindAPI(EGL_OPENGL_API);
	if (!eglChooseConfig(dpy, cfgattr, &cfg, 1, &n) || n < 1) {
		printf("eglChooseConfig failed\n"); return 1; }
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
	if (ctx == EGL_NO_CONTEXT) { printf("eglCreateContext failed\n"); return 1; }

	/* Ladybird's exact call: surfaceless make-current, no pbuffer, no window. */
	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		printf("surfaceless eglMakeCurrent failed\n"); return 1; }
	ok("surfaceless eglMakeCurrent", 1, 0);

	/* Without a current context these GL calls are no-ops (the bug this guards). */
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	ok("glTexImage2D", glGetError() == GL_NO_ERROR, 0);

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	snprintf(d, sizeof d, "status=0x%x (want 0x%x COMPLETE)", status, GL_FRAMEBUFFER_COMPLETE);
	ok("FBO complete", status == GL_FRAMEBUFFER_COMPLETE, d);

	glViewport(0, 0, W, H);
	glClearColor(0.25f, 0.50f, 0.75f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	ok("glClear on FBO", glGetError() == GL_NO_ERROR, 0);

	memset(px, 0xAB, sizeof px);   /* poison: zeros must come from the driver */
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	ok("glReadPixels", glGetError() == GL_NO_ERROR, 0);

	snprintf(d, sizeof d, "got RGBA(%d,%d,%d,%d) want ~(64,128,191,255)",
	         px[0], px[1], px[2], px[3]);
	ok("surfaceless FBO readback has the cleared colour",
	   px[0] > 54 && px[0] < 74 && px[1] > 118 && px[1] < 138 &&
	   px[2] > 181 && px[2] < 201 && px[3] == 255, d);

	printf("09_fbo_surfaceless %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
