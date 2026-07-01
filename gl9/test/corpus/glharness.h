/* glharness.h — shared scaffolding for the gl9 parity corpus. Each corpus program
 * makes an OSMesa 3.3 context, renders modern GL (VBO+VAO+GLSL) into gl9_buf, and
 * writes an ASCII P3 PPM (text transfers cleanly over listen1, unlike binary P6).
 * The SAME source compiles for the host golden (gcc + the Mesa 24.0.9 oracle) and
 * for 9front (cc9), so any pixel difference is softpipe/openlibm-vs-host-libm, not
 * a different program. Keep renders small — they cat back over a flaky listener. */
#ifndef GL9_HARNESS_H
#define GL9_HARNESS_H
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/osmesa.h>
#include <GL/gl.h>
#include <GL/glext.h>

/* immediate stderr trace (survives a fault). Off unless GL9_TRACE is set — was
 * how we localized the glDrawArrays NX-fault (fixed by GALLIUM_NOSSE=1). */
static int gl9_tron = -1;
static void gl9_trace(const char *s){
	if (gl9_tron < 0) gl9_tron = getenv("GL9_TRACE") ? 1 : 0;
	if (!gl9_tron) return;
	write(2, s, (int)__builtin_strlen(s)); write(2, "\n", 1);
}

#ifndef GL9_W
#define GL9_W 64
#endif
#ifndef GL9_H
#define GL9_H 64
#endif

static unsigned char gl9_buf[GL9_W * GL9_H * 4];
static OSMesaContext gl9_ctx;

/* 3.3 context with 24-bit depth + 8-bit stencil; render top-down (row 0 = top)
 * so the PPM needs no flip and host/target agree on orientation. */
static int
gl9_init(void)
{
	gl9_ctx = OSMesaCreateContextExt(OSMESA_RGBA, 24, 8, 0, NULL);
	if (!gl9_ctx) { fprintf(stderr, "OSMesaCreateContextExt failed\n"); return 0; }
	if (!OSMesaMakeCurrent(gl9_ctx, gl9_buf, GL_UNSIGNED_BYTE, GL9_W, GL9_H)) {
		fprintf(stderr, "OSMesaMakeCurrent failed\n"); return 0;
	}
	OSMesaPixelStore(OSMESA_Y_UP, 0);
	glViewport(0, 0, GL9_W, GL9_H);
	return 1;
}

static GLuint
gl9_shader(GLenum type, const char *src)
{
	GLuint s;
	GLint ok;
	gl9_trace("  glCreateShader");
	s = glCreateShader(type);
	gl9_trace("  glShaderSource");
	glShaderSource(s, 1, &src, NULL);
	gl9_trace("  glCompileShader");
	glCompileShader(s);
	gl9_trace("  glGetShaderiv");
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) { char log[1024]; glGetShaderInfoLog(s, 1024, NULL, log);
		fprintf(stderr, "shader compile: %s\n", log); }
	gl9_trace("  shader done");
	return s;
}

/* build+link a program from vertex+fragment source. Use layout(location=) in the
 * shaders for attribute slots (GLSL 330), so no glBindAttribLocation needed. */
static GLuint
gl9_program(const char *vs, const char *fs)
{
	GLuint p, v, f;
	GLint ok;
	gl9_trace(" gl9_program: vertex");
	v = gl9_shader(GL_VERTEX_SHADER, vs);
	gl9_trace(" gl9_program: fragment");
	f = gl9_shader(GL_FRAGMENT_SHADER, fs);
	gl9_trace(" glCreateProgram");
	p = glCreateProgram();
	gl9_trace(" glAttachShader x2");
	glAttachShader(p, v);
	glAttachShader(p, f);
	gl9_trace(" glLinkProgram");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) { char log[1024]; glGetProgramInfoLog(p, 1024, NULL, log);
		fprintf(stderr, "program link: %s\n", log); }
	gl9_trace(" program done");
	return p;
}

static void
gl9_write_ppm(const char *path)
{
	FILE *f = fopen(path, "w");
	int x, y;
	long rs = 0, gs = 0, bs = 0;
	if (!f) { fprintf(stderr, "open %s failed\n", path); return; }
	fprintf(f, "P3\n%d %d\n255\n", GL9_W, GL9_H);
	for (y = 0; y < GL9_H; y++) {
		for (x = 0; x < GL9_W; x++) {
			unsigned char *p = &gl9_buf[(y * GL9_W + x) * 4];
			fprintf(f, "%d %d %d ", p[0], p[1], p[2]);
			rs += p[0]; gs += p[1]; bs += p[2];
		}
		fprintf(f, "\n");
	}
	fclose(f);
	/* one-line signature to stdout: reliable even if the PPM cat-back is flaky */
	long n = (long)GL9_W * GL9_H;
	printf("SIG mean=%ld,%ld,%ld px=%ldx%d\n", rs / n, gs / n, bs / n, (long)GL9_W, GL9_H);
}

/* gl9_present — write the framebuffer to fd as a framed RGBA blob for gl9win to
 * blit (the two-process seam, since cc9 can't link kencc libdraw). Format:
 * "GL9F" | u32 be width | u32 be height | width*height*4 bytes RGBA (OSMesa order). */
static void
gl9_put32(int fd, unsigned v)
{
	unsigned char b[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16),
			       (unsigned char)(v >> 8), (unsigned char)v };
	write(fd, b, 4);
}
static void
gl9_present(int fd)
{
	write(fd, "GL9F", 4);
	gl9_put32(fd, GL9_W);
	gl9_put32(fd, GL9_H);
	write(fd, gl9_buf, (int)(GL9_W * GL9_H * 4));
}

/* corpus programs use this default main (init -> render -> write PPM); windowed
 * demos #define GL9_NO_MAIN and supply their own. */
#ifndef GL9_NO_MAIN
static void gl9_render(void);
int
main(int argc, char **argv)
{
	if (!gl9_init())
		return 1;
	gl9_render();
	glFinish();
	gl9_write_ppm(argc > 1 ? argv[1] : "/tmp/gl9.ppm");
	return 0;
}
#endif
#endif
