/* gl9egl_platform.h — the Plan 9 "native window" gl9's EGL uses. A real winit
 * Plan 9 backend would populate this from a rio window; the demo builds one
 * directly. Kept trivial: eglSwapBuffers presents the surface to gl9win over a
 * pipe, so all the window needs to carry is its size. */
#ifndef GL9EGL_PLATFORM_H
#define GL9EGL_PLATFORM_H

struct gl9_native_win {
	int w, h;
};

#endif
