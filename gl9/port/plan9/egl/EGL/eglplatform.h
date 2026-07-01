/* gl9's EGL platform header — replaces Mesa's eglplatform.h, which #errors on a
 * platform it doesn't recognize (9front is none of X11/Wayland/Win/Android). We
 * have no window system in EGL: the native handle types are opaque pointers and
 * presentation is gl9egl's pipe-to-gl9win. Found before Mesa's copy via the shim
 * include dir; KHR/khrplatform.h + EGL/egl.h still come from Mesa. */
#ifndef __eglplatform_h_
#define __eglplatform_h_

#include <KHR/khrplatform.h>

#ifndef EGLAPI
#define EGLAPI KHRONOS_APICALL
#endif
#ifndef EGLAPIENTRY
#define EGLAPIENTRY KHRONOS_APIENTRY
#endif
#define EGLAPIENTRYP EGLAPIENTRY *

typedef void *EGLNativeDisplayType;
typedef void *EGLNativePixmapType;
typedef void *EGLNativeWindowType;

typedef EGLNativeDisplayType NativeDisplayType;
typedef EGLNativePixmapType NativePixmapType;
typedef EGLNativeWindowType NativeWindowType;

typedef khronos_int32_t EGLint;

/* EGL_CAST — egl.h uses it for EGL_DEFAULT_DISPLAY / EGL_NO_* sentinels. */
#if defined(__cplusplus)
#define EGL_CAST(type, value) (static_cast<type>(value))
#else
#define EGL_CAST(type, value) ((type)(value))
#endif

#endif /* __eglplatform_h_ */
