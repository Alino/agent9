//! The OpenGL platform display selection and creation. Signature-mirrors real
//! glutin 0.32.3 `display.rs` for the surface alacritty touches.

use std::ffi::{self, CStr};
use std::marker::PhantomData;

use raw_window_handle::RawDisplayHandle;

use crate::api::egl::display::Display as EglDisplay;
use crate::api::egl::ffi as egl_ffi;
use crate::api::egl::last_egl_error;
use crate::config::{Config, ConfigTemplate};
use crate::context::{ContextAttributes, NotCurrentContext};
use crate::error::{ErrorKind, Result};
use crate::surface::{Surface, SurfaceAttributes, WindowSurface};

/// A trait to group common display operations.
pub trait GlDisplay {
    /// A window surface created by the display.
    type WindowSurface;
    /// A config that is used by the display.
    type Config;
    /// A context that is being used by the display.
    type NotCurrentContext;

    /// Find configurations matching the given `template`.
    ///
    /// # Safety
    ///
    /// On Plan 9 this is actually safe; `unsafe` is kept for signature
    /// fidelity with real glutin.
    unsafe fn find_configs(
        &self,
        template: ConfigTemplate,
    ) -> Result<Box<dyn Iterator<Item = Self::Config> + '_>>;

    /// Create the graphics platform context.
    ///
    /// # Safety
    ///
    /// See [`GlDisplay::find_configs`].
    unsafe fn create_context(
        &self,
        config: &Self::Config,
        context_attributes: &ContextAttributes,
    ) -> Result<Self::NotCurrentContext>;

    /// Create the surface that can be used to render into a native window.
    ///
    /// # Safety
    ///
    /// See [`GlDisplay::find_configs`].
    unsafe fn create_window_surface(
        &self,
        config: &Self::Config,
        surface_attributes: &SurfaceAttributes<WindowSurface>,
    ) -> Result<Self::WindowSurface>;

    /// Return the address of an OpenGL function.
    fn get_proc_address(&self, addr: &CStr) -> *const ffi::c_void;

    /// Helper to obtain information about the underlying display.
    fn version_string(&self) -> String;

    /// Get the features supported by the display.
    fn supported_features(&self) -> DisplayFeatures;
}

/// Get the [`Display`].
pub trait GetGlDisplay {
    /// The display used by the object.
    type Target;

    /// Obtain the GL display used to create a particular GL object.
    fn display(&self) -> Self::Target;
}

/// The graphics display. On Plan 9 the only backend is gl9egl.
#[derive(Debug, Clone)]
pub enum Display {
    /// The EGL display.
    Egl(EglDisplay),
}

impl Display {
    /// Create a graphics platform display from the given raw display handle.
    ///
    /// The handle is ignored on Plan 9: gl9egl has one process-wide display.
    ///
    /// # Safety
    ///
    /// Actually safe here; `unsafe` kept for signature fidelity.
    pub unsafe fn new(
        _display: RawDisplayHandle,
        _preference: DisplayApiPreference,
    ) -> Result<Self> {
        Ok(Self::Egl(EglDisplay::new()?))
    }
}

impl GlDisplay for Display {
    type Config = Config;
    type NotCurrentContext = NotCurrentContext;
    type WindowSurface = Surface<WindowSurface>;

    unsafe fn find_configs(
        &self,
        _template: ConfigTemplate,
    ) -> Result<Box<dyn Iterator<Item = Self::Config> + '_>> {
        let Self::Egl(display) = self;
        let attrs = [egl_ffi::EGL_NONE];
        let mut config: egl_ffi::EGLConfig = std::ptr::null_mut();
        let mut num = 0i32;
        let ok = unsafe {
            egl_ffi::eglChooseConfig(
                display.raw as egl_ffi::EGLDisplay,
                attrs.as_ptr(),
                &mut config,
                1,
                &mut num,
            )
        };
        if ok == egl_ffi::EGL_FALSE || num < 1 {
            return Err(last_egl_error());
        }
        let config = Config::Egl(crate::api::egl::config::Config {
            raw: config as usize,
            display: display.raw,
        });
        Ok(Box::new(std::iter::once(config)))
    }

    unsafe fn create_context(
        &self,
        config: &Self::Config,
        _context_attributes: &ContextAttributes,
    ) -> Result<Self::NotCurrentContext> {
        let (Self::Egl(display), Config::Egl(config)) = (self, config);
        // gl9egl ignores the attributes: every context is desktop OpenGL over
        // OSMesa. Alacritty's first attempt (OpenGL 3.3 Core) just succeeds.
        let ctx = unsafe {
            egl_ffi::eglCreateContext(
                display.raw as egl_ffi::EGLDisplay,
                config.raw as egl_ffi::EGLConfig,
                std::ptr::null_mut(),
                std::ptr::null(),
            )
        };
        if ctx.is_null() {
            return Err(last_egl_error());
        }
        Ok(NotCurrentContext::Egl(crate::api::egl::context::NotCurrentContext {
            raw: ctx as usize,
            display: display.raw,
            config: config.raw,
        }))
    }

    unsafe fn create_window_surface(
        &self,
        config: &Self::Config,
        surface_attributes: &SurfaceAttributes<WindowSurface>,
    ) -> Result<Self::WindowSurface> {
        let (Self::Egl(display), Config::Egl(config)) = (self, config);
        let (width, height) = match (surface_attributes.width, surface_attributes.height) {
            (Some(w), Some(h)) => (w.get(), h.get()),
            _ => return Err(ErrorKind::BadParameter.into()),
        };
        // gl9egl copies w/h out of the native window synchronously, so a
        // stack temporary is all the "native window" that's needed.
        let native = egl_ffi::Gl9NativeWin { w: width as i32, h: height as i32 };
        let surface = unsafe {
            egl_ffi::eglCreateWindowSurface(
                display.raw as egl_ffi::EGLDisplay,
                config.raw as egl_ffi::EGLConfig,
                &native as *const egl_ffi::Gl9NativeWin as egl_ffi::EGLNativeWindowType,
                std::ptr::null(),
            )
        };
        if surface.is_null() {
            return Err(last_egl_error());
        }
        Ok(Surface::Egl(crate::api::egl::surface::Surface {
            raw: surface as usize,
            display: display.raw,
            _ty: PhantomData,
        }))
    }

    fn get_proc_address(&self, addr: &CStr) -> *const ffi::c_void {
        unsafe { egl_ffi::eglGetProcAddress(addr.as_ptr()) }
    }

    fn version_string(&self) -> String {
        let Self::Egl(display) = self;
        display.version_string()
    }

    fn supported_features(&self) -> DisplayFeatures {
        // No robustness, no damage, no srgb control: alacritty then requests
        // plain NotRobust contexts and takes the plain swap_buffers path.
        DisplayFeatures::SWAP_CONTROL
    }
}

/// Preference of the display that should be used. Only EGL exists on Plan 9.
#[derive(Debug)]
pub enum DisplayApiPreference {
    /// Use EGL (gl9egl).
    Egl,
}

/// The features and extensions supported by the [`Display`].
/// Bitflags-compatible surface without the bitflags dependency.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct DisplayFeatures(u32);

impl DisplayFeatures {
    /// The display supports creating robust contexts.
    pub const CONTEXT_ROBUSTNESS: Self = Self(0b0000_0001);
    /// The display supports creating no-error contexts.
    pub const CONTEXT_NO_ERROR: Self = Self(0b0000_0010);
    /// The display supports floating pixel formats.
    pub const FLOAT_PIXEL_FORMAT: Self = Self(0b0000_0100);
    /// The display supports changing the swap interval on surfaces.
    pub const SWAP_CONTROL: Self = Self(0b0000_1000);
    /// The display supports contexts with explicit release behavior.
    pub const CONTEXT_RELEASE_BEHAVIOR: Self = Self(0b0001_0000);
    /// The display supports creating OpenGL ES contexts.
    pub const CREATE_ES_CONTEXT: Self = Self(0b0010_0000);
    /// The display supports multisampled pixel formats.
    pub const MULTISAMPLING_PIXEL_FORMATS: Self = Self(0b0100_0000);
    /// The display supports SRGB framebuffer surfaces.
    pub const SRGB_FRAMEBUFFERS: Self = Self(0b1000_0000);

    /// No features.
    pub fn empty() -> Self {
        Self(0)
    }

    /// Whether all bits in `other` are set.
    pub fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }
}

impl std::ops::BitOr for DisplayFeatures {
    type Output = Self;

    fn bitor(self, rhs: Self) -> Self {
        Self(self.0 | rhs.0)
    }
}
