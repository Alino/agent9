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
 * contexts across displays, pbuffer readback, sync objects. EGLImage: only the
 * EGL_GL_TEXTURE_2D_KHR form, and only as a handle — see eglCreateImageKHR. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/osmesa.h>
#include <GL/gl.h>
#include "gl9egl_platform.h"

/* gl9 additions to vendored osmesa.c (damage-rect flush_front snapshot) */
extern void OSMesaGL9SetDamage(int x, int y, int w, int h);
extern void OSMesaGL9ClearDamage(void);

extern long write(int, const void *, long);
extern void abort(void);
extern void *malloc(unsigned long);
extern void free(void *);
extern void *memcpy(void *, const void *, unsigned long);
extern void *memmove(void *, const void *, unsigned long);

/* one process-wide display, one config. */
struct gl9_ctx  { OSMesaContext os; EGLint major, minor; };
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

/* Honours the requested version/profile instead of ignoring `attrs`.
 *
 * This used to be `(void)attrs` + OSMesaCreateContextExt, i.e. whatever OSMesa
 * felt like giving. That silently lies to the client twice over: a request for a
 * version we cannot provide (say GL 4.5) came back "successful" as a 3.3 context,
 * and eglQueryContext then had nothing true to report. OSMesa's own contract is
 * the EGL one — "we return a context version >= what you asked for ... otherwise
 * null if the version/profile is not supported" — so passing the request through
 * gets both the version and the failure right for free.
 *
 * Unspecified attributes keep EGL's defaults (major 1, compatibility profile),
 * which OSMesa satisfies with the same context it always handed back. */
EGLContext
eglCreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share,
		 const EGLint *attrs)
{
	struct gl9_ctx *c;
	OSMesaContext sh = share ? ((struct gl9_ctx *)share)->os : 0;
	EGLint major = 1, minor = 0;          /* EGL defaults */
	int profile = OSMESA_COMPAT_PROFILE;  /* EGL default is compatibility */
	(void)dpy; (void)cfg;

	for (const EGLint *a = attrs; a && *a != EGL_NONE; a += 2) {
		switch (a[0]) {
		/* EGL_CONTEXT_CLIENT_VERSION and EGL_CONTEXT_MAJOR_VERSION are the same
		 * token (0x3098); the former is just the EGL 1.4 spelling. */
		case EGL_CONTEXT_MAJOR_VERSION: major = a[1]; break;
		case EGL_CONTEXT_MINOR_VERSION: minor = a[1]; break;
		case EGL_CONTEXT_OPENGL_PROFILE_MASK:
			profile = (a[1] & EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT)
			        ? OSMESA_CORE_PROFILE : OSMESA_COMPAT_PROFILE;
			break;
		default: break;   /* debug/robustness flags: nothing to honour here */
		}
	}

	c = malloc(sizeof *c);
	if (!c) { set_err(EGL_BAD_ALLOC); return EGL_NO_CONTEXT; }

	const int osattrs[] = {
		OSMESA_FORMAT,                OSMESA_RGBA,
		OSMESA_DEPTH_BITS,            24,
		OSMESA_STENCIL_BITS,          8,
		OSMESA_ACCUM_BITS,            0,
		OSMESA_PROFILE,               profile,
		OSMESA_CONTEXT_MAJOR_VERSION, major,
		OSMESA_CONTEXT_MINOR_VERSION, minor,
		0
	};
	c->os = OSMesaCreateContextAttribs(osattrs, sh);
	if (!c->os) { free(c); set_err(EGL_BAD_MATCH); return EGL_NO_CONTEXT; }
	c->major = major;
	c->minor = minor;
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

/* EGL 1.5's spelling of the same call. The native window is platform-defined
 * either way, and on gl9 both mean "a struct gl9_native_win*" — the attrib list
 * (EGLAttrib here, EGLint above) carries nothing we honour on a window. */
EGLSurface
eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig cfg, void *win,
			       const EGLAttrib *attrs)
{
	(void)attrs;
	return eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)win, 0);
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

static struct gl9_ctx *cur_ctx;
static struct gl9_surf *cur_surf;
static struct gl9_surf *dummy_surf;   /* surfaceless make-current backing */

EGLBoolean
eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
	struct gl9_ctx *c = ctx;
	struct gl9_surf *s = draw;
	(void)dpy; (void)read;
	if (!c) return EGL_TRUE;             /* release current: no-op */
	if (!s) {
		/* EGL_KHR_surfaceless_context: no draw surface, the client renders
		 * entirely into an FBO (Ladybird's WebGL CPU-readback path). OSMesa
		 * still needs *a* buffer to make a context current, so bind a
		 * persistent 1x1 throwaway — the FBO is the real render target. */
		if (!dummy_surf && !(dummy_surf = make_surface(1, 1, 0))) {
			set_err(EGL_BAD_ALLOC); return EGL_FALSE;
		}
		s = dummy_surf;
	}
	if (!OSMesaMakeCurrent(c->os, s->buf, GL_UNSIGNED_BYTE, s->w, s->h)) {
		set_err(EGL_BAD_MATCH); return EGL_FALSE;
	}
	OSMesaPixelStore(OSMESA_Y_UP, 0);
	glViewport(0, 0, s->w, s->h);
	cur_ctx = c;
	cur_surf = s;
	return EGL_TRUE;
}

/* The current-binding queries. EGL makes the current context per-thread; these
 * report the process-wide binding that eglMakeCurrent above already keeps, so a
 * second thread making current changes what the first one sees here.
 * ponytail: single global binding, matching eglMakeCurrent. Make cur_ctx/cur_surf
 * __thread if a client ever drives GL from more than one thread at a time. */
EGLContext eglGetCurrentContext(void) { return (EGLContext)cur_ctx; }

EGLDisplay
eglGetCurrentDisplay(void)
{
	return cur_ctx ? GL9_DISPLAY : EGL_NO_DISPLAY;
}

EGLSurface
eglGetCurrentSurface(EGLint readdraw)
{
	/* eglMakeCurrent binds draw to OSMesa and ignores read, so read and draw
	 * are the same surface here; answer both the same way rather than claim a
	 * separate read surface that does not exist. */
	(void)readdraw;
	return (EGLSurface)cur_surf;
}

/* EGLImage (EGL_KHR_image_base + EGL_KHR_gl_texture_2D_image), enough for
 * surfman, which types these three as plain function pointers under the comment
 * "ubiquitous extensions assumed to be present" — so a missing one is not a
 * graceful fallback, it is a call through NULL.
 *
 * Only EGL_GL_TEXTURE_2D_KHR is supported, and for that target the EGLImage IS
 * the GL texture: the handle is the texture name. That is exactly what the
 * extension says the image is, so no bookkeeping is needed and destroy has
 * nothing to free (the texture belongs to whoever created it). Texture names
 * start at 1, so the handle is never NULL for a valid texture. */
EGLImageKHR
eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target,
		  EGLClientBuffer buffer, const EGLint *attrs)
{
	(void)ctx; (void)attrs;   /* EGL_IMAGE_PRESERVED_KHR: we alias, never copy */
	if(dpy != GL9_DISPLAY)            { set_err(EGL_BAD_DISPLAY);   return EGL_NO_IMAGE_KHR; }
	if(target != EGL_GL_TEXTURE_2D_KHR){ set_err(EGL_BAD_PARAMETER); return EGL_NO_IMAGE_KHR; }
	if(!buffer)                       { set_err(EGL_BAD_PARAMETER); return EGL_NO_IMAGE_KHR; }
	return (EGLImageKHR)buffer;       /* the texture name, unchanged */
}

EGLBoolean
eglDestroyImageKHR(EGLDisplay dpy, EGLImageKHR image)
{
	(void)image;
	if(dpy != GL9_DISPLAY){ set_err(EGL_BAD_DISPLAY); return EGL_FALSE; }
	return EGL_TRUE;      /* the image held no state of its own */
}

/* GL_OES_EGL_image: re-point the bound texture at the image's storage.
 *
 * This one cannot be honoured. It must make the currently bound texture ALIAS
 * another texture's storage, and desktop GL has no such operation before
 * glTextureView/glCopyImageSubData (GL 4.x; softpipe here is 3.3). Copying
 * instead would look right once and then silently go stale the next time the
 * producer drew — a wrong frame is worse than a stopped process, and the
 * function returns void so it cannot report failure to the caller.
 *
 * Nothing in a plain page reaches this: surfman only calls it from
 * to_surface_texture, which Servo uses to composite WebGL/canvas surfaces as
 * WebRender external images. If this fires, that is the feature that needs a
 * real answer (make the consumer share the producer's context and hand back the
 * producer's own texture, which for THIS image type is exactly `image`). */
void
glEGLImageTargetTexture2DOES(GLenum target, GLeglImageOES image)
{
	static const char msg[] =
	    "gl9egl: glEGLImageTargetTexture2DOES is unimplemented — GL 3.3 cannot "
	    "alias texture storage, and copying would silently serve stale frames. "
	    "See the comment in gl9egl.c.\n";
	(void)target; (void)image;
	write(2, msg, sizeof msg - 1);
	abort();
}

EGLBoolean
eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attr, EGLint *val)
{
	if (dpy != GL9_DISPLAY) { set_err(EGL_BAD_DISPLAY); return EGL_FALSE; }
	if (!ctx)               { set_err(EGL_BAD_CONTEXT); return EGL_FALSE; }
	if (!val)               return EGL_FALSE;
	switch (attr) {
	/* There is exactly one config (id 1, see eglGetConfigAttrib). */
	case EGL_CONFIG_ID:             *val = 1; break;
	case EGL_CONTEXT_CLIENT_TYPE:   *val = EGL_OPENGL_API; break;
	/* EGL_CONTEXT_CLIENT_VERSION is the same token as EGL_CONTEXT_MAJOR_VERSION
	 * and applies to OpenGL as well as ES, so report the version the context was
	 * actually created with. (It answered 0 here, on the wrong belief that the
	 * attribute was ES-only.) */
	case EGL_CONTEXT_MAJOR_VERSION: *val = ((struct gl9_ctx *)ctx)->major; break;
	case EGL_CONTEXT_MINOR_VERSION: *val = ((struct gl9_ctx *)ctx)->minor; break;
	case EGL_RENDER_BUFFER:         *val = EGL_BACK_BUFFER; break;
	default:                        set_err(EGL_BAD_ATTRIBUTE); return EGL_FALSE;
	}
	return EGL_TRUE;
}

/* gl9 extension (no EGL equivalent): resize a surface in place. OSMesa renders
 * into our client buffer, so resizing = realloc + rebind. Re-binds the current
 * context itself so callers (the glutin shim) need no extra ceremony. */
int
gl9egl_surface_resize(EGLSurface surf, int w, int h)
{
	struct gl9_surf *s = surf;
	unsigned char *nbuf;

	if (!s || w <= 0 || h <= 0) return 0;
	nbuf = malloc((unsigned long)w * h * 4);
	if (!nbuf) { set_err(EGL_BAD_ALLOC); return 0; }
	free(s->buf);
	s->buf = nbuf;
	s->w = w;
	s->h = h;
	if (cur_surf == s && cur_ctx) {
		if (!OSMesaMakeCurrent(cur_ctx->os, s->buf, GL_UNSIGNED_BYTE, w, h))
			return 0;
		OSMesaPixelStore(OSMESA_Y_UP, 0);
		glViewport(0, 0, w, h);
	}
	return 1;
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

/* gl9 extension (no dlopen'able EGL ext mechanism here): present only a
 * damaged region. Coordinates arrive in EGL convention (origin bottom-left,
 * KHR_swap_buffers_with_damage); we flip to the top-left rows OSMesa's
 * Y_UP=0 buffer uses and emit "GL9D" | x | y | w | h | w*h*4 RGBA to the
 * window host. A full-window blit to a real framebuffer costs 100x a small
 * one, so this is the interactive-latency path (typing = a few rows). */
int
gl9egl_swap_damage(EGLSurface surf, int x, int y, int w, int h)
{
	struct gl9_surf *s = surf;
	unsigned char *rows;
	int i;

	if (!s || !s->window) return 0;
	/* flip to top-left origin, then clamp */
	y = s->h - (y + h);
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > s->w) w = s->w - x;
	if (y + h > s->h) h = s->h - y;
	if (w <= 0 || h <= 0) { glFinish(); return 1; } /* nothing visible changed */
	if (w == s->w && h == s->h)          /* full frame: use the plain path */
		return eglSwapBuffers(GL9_DISPLAY, surf);
	/* tell OSMesa's flush_front to snapshot only this rect — the
	 * full-surface copy is ~20ms at 940x640, the rect is ~free */
	OSMesaGL9SetDamage(x, y, w, h);
	glFinish();
	OSMesaGL9ClearDamage();
	rows = malloc((unsigned long)w * h * 4);
	if (!rows)
		return eglSwapBuffers(GL9_DISPLAY, surf);
	for (i = 0; i < h; i++)
		memcpy(rows + (unsigned long)i * w * 4,
		       s->buf + ((unsigned long)(y + i) * s->w + x) * 4,
		       (unsigned long)w * 4);
	write(s->fd, "GL9D", 4);
	put32(s->fd, x);
	put32(s->fd, y);
	put32(s->fd, w);
	put32(s->fd, h);
	write(s->fd, rows, (long)w * h * 4);
	free(rows);
	return 1;
}

/* gl9 extension: rows [y0, y1) scrolled UP by dy pixels (top-left window
 * coordinates, matching the buffer's Y_UP=0 layout). Shifts the OSMesa
 * buffer in place and emits "GL9S" | y0 | y1 | dy so the window host shifts
 * its persistent image and the screen identically — a scroll then costs a
 * blit, not a re-render + 2MB re-send. */
int
gl9egl_scroll(EGLSurface surf, int y0, int y1, int dy)
{
	struct gl9_surf *s = surf;
	int rows;

	if (!s || !s->window || dy <= 0) return 0;
	if (y0 < 0) y0 = 0;
	if (y1 > s->h) y1 = s->h;
	rows = (y1 - y0) - dy;
	if (rows <= 0) return 0;
	memmove(s->buf + (unsigned long)y0 * s->w * 4,
	        s->buf + (unsigned long)(y0 + dy) * s->w * 4,
	        (unsigned long)rows * s->w * 4);
	write(s->fd, "GL9S", 4);
	put32(s->fd, y0);
	put32(s->fd, y1);
	put32(s->fd, dy);
	return 1;
}

/* eglGetProcAddress resolves gl9egl's OWN entry points before asking OSMesa.
 *
 * Forwarding straight to OSMesaGetProcAddress (as this used to) can only ever
 * find GL functions: it knows nothing of EGL, so every eglFoo name came back
 * NULL. Callers that look EGL up dynamically then get a null they may not check
 * — surfman's EGL_EXTENSION_FUNCTIONS types eglCreateImageKHR as a plain fn
 * pointer and calls it, which is a jump to 0. EGL 1.5 says this function
 * resolves EGL, GL and extension entry points; now it does.
 *
 * The table is also how gl9egl's glEGLImageTargetTexture2DOES wins over Mesa's:
 * OSMesa would happily hand back _mesa_EGLImageTargetTexture2DOES, which expects
 * a Mesa EGLImage and would misread our handle (a bare GL texture name). Ours is
 * checked first, so the honest failure is what callers get. */
struct gl9_proc { const char *name; void *fn; };
static const struct gl9_proc gl9_procs[] = {
#define P(f) { #f, (void *)f },
	P(eglBindAPI)
	P(eglChooseConfig)
	P(eglCreateContext)
	P(eglCreateImageKHR)
	P(eglCreatePbufferSurface)
	P(eglCreatePlatformWindowSurface)
	P(eglCreateWindowSurface)
	P(eglDestroyContext)
	P(eglDestroyImageKHR)
	P(eglDestroySurface)
	P(eglGetConfigAttrib)
	P(eglGetConfigs)
	P(eglGetCurrentContext)
	P(eglGetCurrentDisplay)
	P(eglGetCurrentSurface)
	P(eglGetDisplay)
	P(eglGetError)
	P(eglGetPlatformDisplay)
	P(eglGetPlatformDisplayEXT)
	P(eglGetProcAddress)
	P(eglInitialize)
	P(eglMakeCurrent)
	P(eglQueryAPI)
	P(eglQueryContext)
	P(eglQueryString)
	P(eglQuerySurface)
	P(eglSwapBuffers)
	P(eglSwapInterval)
	P(eglTerminate)
	P(glEGLImageTargetTexture2DOES)
#undef P
	{ 0, 0 }
};

static int gl9_streq(const char *a, const char *b)
{
	while(*a && *a == *b){ a++; b++; }
	return *a == *b;
}

__eglMustCastToProperFunctionPointerType
eglGetProcAddress(const char *name)
{
	if(!name) return 0;
	for(const struct gl9_proc *p = gl9_procs; p->name; p++)
		if(gl9_streq(p->name, name))
			return (__eglMustCastToProperFunctionPointerType)p->fn;
	/* Not ours — a GL entry point, which is OSMesa's to answer. */
	return (__eglMustCastToProperFunctionPointerType)OSMesaGetProcAddress(name);
}
