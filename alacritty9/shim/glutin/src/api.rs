//! The platform apis. On Plan 9 the only backend is EGL — gl9egl, a minimal
//! statically-linked libEGL over OSMesa.

pub mod egl {
    //! The gl9egl backend. All EGL entrypoints are resolved at static link
    //! time against gl9egl.a; there is no library loading.

    use crate::error::{Error, ErrorKind};

    pub(crate) mod ffi {
        //! Standard EGL 1.4 signatures over plain C typedefs. Handles are
        //! kept as `usize` in the wrappers (Send/Sync for free) and cast to
        //! pointers at the call boundary. No libc crate — it doesn't exist
        //! for plan9.
        #![allow(non_snake_case)]

        use std::ffi::{c_char, c_int, c_uint, c_void};

        pub type EGLBoolean = c_uint;
        pub type EGLint = i32;
        pub type EGLenum = c_uint;
        pub type EGLDisplay = *mut c_void;
        pub type EGLConfig = *mut c_void;
        pub type EGLContext = *mut c_void;
        pub type EGLSurface = *mut c_void;
        pub type EGLNativeDisplayType = *mut c_void;
        pub type EGLNativeWindowType = *mut c_void;

        pub const EGL_FALSE: EGLBoolean = 0;
        pub const EGL_NONE: EGLint = 0x3038;
        pub const EGL_VERSION: EGLint = 0x3054;
        pub const EGL_OPENGL_API: EGLenum = 0x30A2;

        /// `struct gl9_native_win` from gl9egl_platform.h — all the "native
        /// window" carries on Plan 9 is its size.
        #[repr(C)]
        pub struct Gl9NativeWin {
            pub w: c_int,
            pub h: c_int,
        }

        extern "C" {
            pub fn eglGetDisplay(display: EGLNativeDisplayType) -> EGLDisplay;
            pub fn eglInitialize(
                dpy: EGLDisplay,
                major: *mut EGLint,
                minor: *mut EGLint,
            ) -> EGLBoolean;
            pub fn eglTerminate(dpy: EGLDisplay) -> EGLBoolean;
            pub fn eglQueryString(dpy: EGLDisplay, name: EGLint) -> *const c_char;
            pub fn eglBindAPI(api: EGLenum) -> EGLBoolean;
            pub fn eglChooseConfig(
                dpy: EGLDisplay,
                attrs: *const EGLint,
                configs: *mut EGLConfig,
                config_size: EGLint,
                num_config: *mut EGLint,
            ) -> EGLBoolean;
            pub fn eglCreateContext(
                dpy: EGLDisplay,
                config: EGLConfig,
                share_context: EGLContext,
                attrs: *const EGLint,
            ) -> EGLContext;
            pub fn eglCreateWindowSurface(
                dpy: EGLDisplay,
                config: EGLConfig,
                win: EGLNativeWindowType,
                attrs: *const EGLint,
            ) -> EGLSurface;
            pub fn eglMakeCurrent(
                dpy: EGLDisplay,
                draw: EGLSurface,
                read: EGLSurface,
                ctx: EGLContext,
            ) -> EGLBoolean;
            pub fn eglSwapBuffers(dpy: EGLDisplay, surface: EGLSurface) -> EGLBoolean;
            pub fn eglSwapInterval(dpy: EGLDisplay, interval: EGLint) -> EGLBoolean;
            pub fn eglGetError() -> EGLint;
            pub fn eglGetProcAddress(procname: *const c_char) -> *const c_void;

            /// gl9egl extension (added by a parallel workstream): resize the
            /// backing buffer of a window surface. Declared here so
            /// `Surface::resize` binds it; if it's missing at final link
            /// that's a link-time error to fix there, not here.
            pub fn gl9egl_surface_resize(surface: EGLSurface, w: c_int, h: c_int);
            pub fn gl9egl_swap_damage(
                surface: EGLSurface,
                x: c_int,
                y: c_int,
                w: c_int,
                h: c_int,
            ) -> c_int;
        }
    }

    /// Map `eglGetError()` to a glutin [`Error`].
    pub(crate) fn last_egl_error() -> Error {
        let code = unsafe { ffi::eglGetError() };
        let kind = match code {
            0x3000 => ErrorKind::Misc, // EGL_SUCCESS but the call failed
            0x3001 => ErrorKind::InitializationFailed,
            0x3002 => ErrorKind::BadAccess,
            0x3003 => ErrorKind::OutOfMemory,
            0x3004 => ErrorKind::BadAttribute,
            0x3005 => ErrorKind::BadConfig,
            0x3006 => ErrorKind::BadContext,
            0x3007 => ErrorKind::BadCurrentSurface,
            0x3008 => ErrorKind::BadDisplay,
            0x3009 => ErrorKind::BadMatch,
            0x300A => ErrorKind::BadNativePixmap,
            0x300B => ErrorKind::BadNativeWindow,
            0x300C => ErrorKind::BadParameter,
            0x300D => ErrorKind::BadSurface,
            0x300E => ErrorKind::ContextLost,
            _ => ErrorKind::Misc,
        };
        Error::new(Some(code as i64), None, kind)
    }

    pub mod display {
        //! The EGL display.

        use super::ffi;
        use crate::error::{ErrorKind, Result};

        /// The gl9egl display (process-wide singleton on the C side).
        #[derive(Debug, Clone)]
        pub struct Display {
            pub(crate) raw: usize,
        }

        impl Display {
            pub(crate) fn new() -> Result<Self> {
                unsafe {
                    let dpy = ffi::eglGetDisplay(std::ptr::null_mut());
                    if dpy.is_null() {
                        return Err(ErrorKind::BadDisplay.into());
                    }
                    let (mut major, mut minor) = (0i32, 0i32);
                    if ffi::eglInitialize(dpy, &mut major, &mut minor) == ffi::EGL_FALSE {
                        return Err(super::last_egl_error());
                    }
                    // Alacritty wants desktop OpenGL contexts.
                    ffi::eglBindAPI(ffi::EGL_OPENGL_API);
                    Ok(Self { raw: dpy as usize })
                }
            }

            pub(crate) fn version_string(&self) -> String {
                unsafe {
                    let s = ffi::eglQueryString(self.raw as ffi::EGLDisplay, ffi::EGL_VERSION);
                    if s.is_null() {
                        String::from("EGL")
                    } else {
                        format!("EGL {}", std::ffi::CStr::from_ptr(s).to_string_lossy())
                    }
                }
            }

            /// Terminate the EGL display.
            ///
            /// # Safety
            ///
            /// No EGL objects may be used after this call.
            pub unsafe fn terminate(self) {
                unsafe {
                    ffi::eglTerminate(self.raw as ffi::EGLDisplay);
                }
            }
        }
    }

    pub mod config {
        //! The EGL config (gl9egl exposes exactly one: RGBA8888/D24/S8).

        /// The single gl9egl config.
        #[derive(Debug, Clone, Copy, PartialEq, Eq)]
        pub struct Config {
            pub(crate) raw: usize,
            pub(crate) display: usize,
        }
    }

    pub mod context {
        //! The EGL contexts.

        use std::sync::atomic::{AtomicUsize, Ordering};

        // ponytail: process-wide "current" tracking (real EGL is per-thread);
        // alacritty drives GL from one thread. Make it thread-local if that
        // ever changes.
        pub(crate) static CURRENT: AtomicUsize = AtomicUsize::new(0);

        pub(crate) fn set_current(ctx: usize) {
            CURRENT.store(ctx, Ordering::Relaxed);
        }

        pub(crate) fn is_current(ctx: usize) -> bool {
            CURRENT.load(Ordering::Relaxed) == ctx
        }

        /// A context known to be not current.
        #[derive(Debug)]
        pub struct NotCurrentContext {
            pub(crate) raw: usize,
            pub(crate) display: usize,
            pub(crate) config: usize,
        }

        /// A context possibly current on the calling thread.
        #[derive(Debug)]
        pub struct PossiblyCurrentContext {
            pub(crate) raw: usize,
            pub(crate) display: usize,
            pub(crate) config: usize,
        }
    }

    pub mod surface {
        //! The EGL surfaces.

        use std::marker::PhantomData;

        use super::context::PossiblyCurrentContext;
        use super::ffi;
        use crate::error::Result;
        use crate::surface::{Rect, SurfaceTypeTrait};

        /// An EGL window surface; `eglSwapBuffers` writes the frame to fd 1
        /// (the gl9win presentation protocol) — that IS the whole
        /// presentation path.
        #[derive(Debug)]
        pub struct Surface<T: SurfaceTypeTrait> {
            pub(crate) raw: usize,
            pub(crate) display: usize,
            pub(crate) _ty: PhantomData<T>,
        }

        impl<T: SurfaceTypeTrait> Surface<T> {
            /// Swap the underlying buffers: present the frame to gl9win.
            pub fn swap_buffers(&self, _context: &PossiblyCurrentContext) -> Result<()> {
                let ok = unsafe {
                    ffi::eglSwapBuffers(
                        self.display as ffi::EGLDisplay,
                        self.raw as ffi::EGLSurface,
                    )
                };
                if ok == ffi::EGL_FALSE {
                    Err(super::last_egl_error())
                } else {
                    Ok(())
                }
            }

            /// Present only the damaged region: the bounding box of `rects`
            /// goes through gl9egl_swap_damage as a "GL9D" sub-rect record.
            /// A full-window blit to a real framebuffer costs ~100x a small
            /// one on 9front, so this is what makes typing feel instant.
            /// Rects are in EGL convention (origin bottom-left); the C side
            /// flips them. Empty damage = full swap (the KHR semantics).
            pub fn swap_buffers_with_damage(
                &self,
                context: &PossiblyCurrentContext,
                rects: &[Rect],
            ) -> Result<()> {
                if rects.is_empty() {
                    return self.swap_buffers(context);
                }
                let mut x0 = i32::MAX;
                let mut y0 = i32::MAX;
                let mut x1 = i32::MIN;
                let mut y1 = i32::MIN;
                for r in rects {
                    x0 = x0.min(r.x);
                    y0 = y0.min(r.y);
                    x1 = x1.max(r.x + r.width);
                    y1 = y1.max(r.y + r.height);
                }
                let ok = unsafe {
                    ffi::gl9egl_swap_damage(
                        self.raw as ffi::EGLSurface,
                        x0,
                        y0,
                        x1 - x0,
                        y1 - y0,
                    )
                };
                if ok == 0 { Err(super::last_egl_error()) } else { Ok(()) }
            }
        }
    }
}
