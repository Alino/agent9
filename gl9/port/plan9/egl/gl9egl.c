/* gl9egl — a minimal libEGL for 9front, backed by OSMesa + softpipe. This is the
 * "EGL seam" of the gl9 plan: glutin (Alacritty's context layer) talks EGL, so we
 * provide the ~20 EGL entrypoints it binds — but over OSMesa instead of porting
 * Mesa's egl_dri2 (DRI loader / platform backends / dlopen), which stock 9front
 * can't support. eglMakeCurrent -> OSMesaMakeCurrent; eglSwapBuffers -> the
 * gl9_present frame protocol into gl9win. Pixel output is the same softpipe that
 * passed the parity suite; EGL is just the binding.
 *
 * Enough of EGL 1.4/1.5 for a glutin-style client: get display, init, choose one
 * RGBA8888/D24/S8 config, create context, create a window/pbuffer surface, make
 * current, swap, get-proc-address. Not implemented: multiple configs, shared
 * contexts across displays, pbuffer readback, EGLImage, sync objects. */
#include <EGL/egl.h>
#include <GL/osmesa.h>
#include <GL/gl.h>
#include "gl9egl_platform.h"

extern long write(int, const void *, long);
extern void *malloc(unsigned long);
extern void free(void *);

/* one process-wide display, one config. */
struct gl9_ctx  { OSMesaContext os; };
struct gl9_surf { int w, h, fd; unsigned char *buf; int window; };

static int g_inited;
static EGLint g_error = EGL_SUCCESS;
static char g_cfg;                       /* the single config; &g_cfg is its handle */
#define GL9_DISPLAY ((EGLDisplay)1)

static void set_err(EGLint e) { g_error = e; }

EGLint eglGetError(void) { EGLint e = g_error; g_error = EGL_SUCCESS; return e; }

EGLDisplay eglGetDisplay(EGLNativeDisplayType d) { (void)d; return GL9_DISPLAY; }

EGLDisplay
eglGetPlatformDisplay(EGLenum platform, void *native, const EGLAttrib *attr)
{ (void)platform; (void)native; (void)attr; return GL9_DISPLAY; }

EGLDisplay
eglGetPlatformDisplayEXT(EGLenum platform, void *native, const EGLint *attr)
{ (void)platform; (void)native; (void)attr; return GL9_DISPLAY; }

EGLBoolean
eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
	if (dpy != GL9_DISPLAY) { set_err(EGL_BAD_DISPLAY); return EGL_FALSE; }
	g_inited = 1;
	if (major) *major = 1;
	if (minor) *minor = 5;
	return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy) { (void)dpy; g_inited = 0; return EGL_TRUE; }

const char *
eglQueryString(EGLDisplay dpy, EGLint name)
{
	(void)dpy;
	switch (name) {
	case EGL_VERSION:    return "1.5 gl9-osmesa";
	case EGL_VENDOR:     return "gl9";
	case EGL_CLIENT_APIS: return "OpenGL OpenGL_ES";
	case EGL_EXTENSIONS: return "EGL_KHR_create_context EGL_EXT_platform_base";
	default: return "";
	}
}

EGLBoolean eglBindAPI(EGLenum api) { (void)api; return EGL_TRUE; }
EGLenum    eglQueryAPI(void) { return EGL_OPENGL_API; }
EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint n) { (void)dpy; (void)n; return EGL_TRUE; }

/* one config; report it for any request. */
static EGLBoolean
fill_configs(EGLConfig *out, EGLint size, EGLint *num)
{
	if (out && size >= 1)
		out[0] = (EGLConfig)&g_cfg;
	if (num) *num = 1;
	return EGL_TRUE;
}
EGLBoolean
eglChooseConfig(EGLDisplay dpy, const EGLint *attrs, EGLConfig *out, EGLint size, EGLint *num)
{ (void)dpy; (void)attrs; return fill_configs(out, size, num); }
EGLBoolean
eglGetConfigs(EGLDisplay dpy, EGLConfig *out, EGLint size, EGLint *num)
{ (void)dpy; return fill_configs(out, size, num); }

EGLBoolean
eglGetConfigAttrib(EGLDisplay dpy, EGLConfig cfg, EGLint attr, EGLint *val)
{
	(void)dpy; (void)cfg;
	switch (attr) {
	case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE:
	case EGL_ALPHA_SIZE:            *val = 8; break;
	case EGL_DEPTH_SIZE:            *val = 24; break;
	case EGL_STENCIL_SIZE:          *val = 8; break;
	case EGL_BUFFER_SIZE:           *val = 32; break;
	case EGL_CONFIG_ID:             *val = 1; break;
	case EGL_NATIVE_VISUAL_ID:      *val = 0; break;
	case EGL_SURFACE_TYPE:          *val = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; break;
	case EGL_RENDERABLE_TYPE:       *val = EGL_OPENGL_BIT | EGL_OPENGL_ES2_BIT; break;
	case EGL_CONFORMANT:            *val = EGL_OPENGL_BIT | EGL_OPENGL_ES2_BIT; break;
	case EGL_CONFIG_CAVEAT:         *val = EGL_NONE; break;
	default:                        *val = 0; break;
	}
	return EGL_TRUE;
}

EGLContext
eglCreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share,
		 const EGLint *attrs)
{
	struct gl9_ctx *c;
	OSMesaContext sh = share ? ((struct gl9_ctx *)share)->os : 0;
	(void)dpy; (void)cfg; (void)attrs;
	c = malloc(sizeof *c);
	if (!c) { set_err(EGL_BAD_ALLOC); return EGL_NO_CONTEXT; }
	c->os = OSMesaCreateContextExt(OSMESA_RGBA, 24, 8, 0, sh);
	if (!c->os) { free(c); set_err(EGL_BAD_MATCH); return EGL_NO_CONTEXT; }
	return (EGLContext)c;
}

EGLBoolean
eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
	(void)dpy;
	if (ctx) { OSMesaDestroyContext(((struct gl9_ctx *)ctx)->os); free(ctx); }
	return EGL_TRUE;
}

static struct gl9_surf *
make_surface(int w, int h, int window)
{
	struct gl9_surf *s = malloc(sizeof *s);
	if (!s) { set_err(EGL_BAD_ALLOC); return 0; }
	s->w = w; s->h = h; s->window = window; s->fd = 1;   /* present to stdout -> gl9win */
	s->buf = malloc((unsigned long)w * h * 4);
	if (!s->buf) { free(s); set_err(EGL_BAD_ALLOC); return 0; }
	return s;
}

/* native window is a struct gl9_native_win* (w,h) — a Plan 9 winit backend would
 * supply it; the demo builds one directly. */
EGLSurface
eglCreateWindowSurface(EGLDisplay dpy, EGLConfig cfg, EGLNativeWindowType win,
		       const EGLint *attrs)
{
	struct gl9_native_win *nw = (struct gl9_native_win *)win;
	(void)dpy; (void)cfg; (void)attrs;
	if (!nw) { set_err(EGL_BAD_NATIVE_WINDOW); return EGL_NO_SURFACE; }
	return (EGLSurface)make_surface(nw->w, nw->h, 1);
}

EGLSurface
eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig cfg, const EGLint *attrs)
{
	int w = 64, h = 64, i;
	(void)dpy; (void)cfg;
	for (i = 0; attrs && attrs[i] != EGL_NONE; i += 2) {
		if (attrs[i] == EGL_WIDTH)  w = attrs[i + 1];
		if (attrs[i] == EGL_HEIGHT) h = attrs[i + 1];
	}
	return (EGLSurface)make_surface(w, h, 0);
}

EGLBoolean
eglDestroySurface(EGLDisplay dpy, EGLSurface surf)
{
	struct gl9_surf *s = surf;
	(void)dpy;
	if (s) { free(s->buf); free(s); }
	return EGL_TRUE;
}

EGLBoolean
eglQuerySurface(EGLDisplay dpy, EGLSurface surf, EGLint attr, EGLint *val)
{
	struct gl9_surf *s = surf;
	(void)dpy;
	if (!s) return EGL_FALSE;
	if (attr == EGL_WIDTH)  *val = s->w;
	else if (attr == EGL_HEIGHT) *val = s->h;
	else *val = 0;
	return EGL_TRUE;
}

EGLBoolean
eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
	struct gl9_ctx *c = ctx;
	struct gl9_surf *s = draw;
	(void)dpy; (void)read;
	if (!c || !s) return EGL_TRUE;      /* release current: no-op */
	if (!OSMesaMakeCurrent(c->os, s->buf, GL_UNSIGNED_BYTE, s->w, s->h)) {
		set_err(EGL_BAD_MATCH); return EGL_FALSE;
	}
	OSMesaPixelStore(OSMESA_Y_UP, 0);
	glViewport(0, 0, s->w, s->h);
	return EGL_TRUE;
}

static void
put32(int fd, unsigned v)
{
	unsigned char b[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16),
			       (unsigned char)(v >> 8), (unsigned char)v };
	write(fd, b, 4);
}

EGLBoolean
eglSwapBuffers(EGLDisplay dpy, EGLSurface surf)
{
	struct gl9_surf *s = surf;
	(void)dpy;
	if (!s) return EGL_FALSE;
	glFinish();
	if (s->window) {                    /* window surface: present a frame to gl9win */
		write(s->fd, "GL9F", 4);
		put32(s->fd, s->w);
		put32(s->fd, s->h);
		write(s->fd, s->buf, (long)s->w * s->h * 4);
	}
	return EGL_TRUE;
}

__eglMustCastToProperFunctionPointerType
eglGetProcAddress(const char *name)
{
	return (__eglMustCastToProperFunctionPointerType)OSMesaGetProcAddress(name);
}
