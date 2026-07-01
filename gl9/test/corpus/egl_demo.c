/* egl_demo — Phase 4 proof: a client that renders via the EGL API (not OSMesa
 * directly), exactly as glutin/Alacritty would. eglGetDisplay -> eglInitialize ->
 * eglChooseConfig -> eglCreateContext -> eglCreateWindowSurface -> eglMakeCurrent
 * -> [modern GL] -> eglSwapBuffers (which presents the frame to gl9win). Under the
 * hood gl9egl runs it on the same softpipe that passed the parity suite. */
#include <stdio.h>
#include <EGL/egl.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include "gl9egl_platform.h"

static const char *VS =
	"#version 330 core\n"
	"layout(location=0) in vec2 pos;\n"
	"layout(location=1) in vec3 col;\n"
	"out vec3 vcol;\n"
	"void main(){ vcol = col; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS =
	"#version 330 core\n"
	"in vec3 vcol;\n"
	"out vec4 o;\n"
	"void main(){ o = vec4(vcol, 1.0); }\n";

static GLuint
prog(void)
{
	GLuint p = glCreateProgram(), v = glCreateShader(GL_VERTEX_SHADER),
	       f = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(v, 1, &VS, 0); glCompileShader(v);
	glShaderSource(f, 1, &FS, 0); glCompileShader(f);
	glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
	return p;
}

int
main(void)
{
	struct gl9_native_win win = { 480, 480 };
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLContext ctx;
	EGLSurface surf;
	EGLint n;
	static const EGLint cfgattr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
	static const EGLint ctxattr[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
					  EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
	static const float v[] = {
		-0.85f, -0.85f,  1, 0, 0,
		 0.85f, -0.85f,  0, 1, 0,
		 0.0f,   0.85f,  0, 0, 1,
	};
	GLuint p, vao, vbo;

	dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(dpy, 0, 0)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
	eglBindAPI(EGL_OPENGL_API);
	if (!eglChooseConfig(dpy, cfgattr, &cfg, 1, &n) || n < 1) {
		fprintf(stderr, "eglChooseConfig failed\n"); return 1; }
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
	if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext failed\n"); return 1; }
	surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)(void *)&win, 0);
	if (surf == EGL_NO_SURFACE) { fprintf(stderr, "eglCreateWindowSurface failed\n"); return 1; }
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "eglMakeCurrent failed\n"); return 1; }

	fprintf(stderr, "EGL up: %s\n", eglQueryString(dpy, EGL_VERSION));
	fprintf(stderr, "GL_VERSION=%s\n", (const char *)glGetString(GL_VERSION));

	glClearColor(0.14f, 0.10f, 0.10f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	p = prog();
	glGenVertexArrays(1, &vao); glBindVertexArray(vao);
	glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(2 * sizeof(float)));
	glEnableVertexAttribArray(1);
	glUseProgram(p);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	eglSwapBuffers(dpy, surf);      /* -> gl9win */
	fprintf(stderr, "egl swapped %dx%d\n", win.w, win.h);
	return 0;
}
