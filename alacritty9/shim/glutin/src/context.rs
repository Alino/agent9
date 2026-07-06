//! OpenGL context creation and initialization. Signature-mirrors real glutin
//! 0.32.3 `context.rs` for the surface alacritty touches.

use raw_window_handle::RawWindowHandle;

use crate::api::egl::context::{
    self as egl_context, NotCurrentContext as NotCurrentEglContext,
    PossiblyCurrentContext as PossiblyCurrentEglContext,
};
use crate::api::egl::ffi;
use crate::api::egl::last_egl_error;
use crate::config::{Config, GetGlConfig};
use crate::display::{Display, GetGlDisplay};
use crate::error::Result;
use crate::surface::{Surface, SurfaceTypeTrait};

/// A trait to group common context operations.
pub trait GlContext {
    /// Get the [`ContextApi`] used by the context.
    fn context_api(&self) -> ContextApi;
}

/// A trait to group common not current operations.
pub trait NotCurrentGlContext {
    /// The type of the possibly current context.
    type PossiblyCurrentContext;

    /// The surface supported by the context.
    type Surface<T: SurfaceTypeTrait>;

    /// Treat the not current context as possibly current.
    fn treat_as_possibly_current(self) -> Self::PossiblyCurrentContext;

    /// Make the context current on the calling thread.
    fn make_current<T: SurfaceTypeTrait>(
        self,
        surface: &Self::Surface<T>,
    ) -> Result<Self::PossiblyCurrentContext>;
}

/// A trait to group common possibly current context operations.
pub trait PossiblyCurrentGlContext {
    /// The not current context type.
    type NotCurrentContext;

    /// The surface supported by the context.
    type Surface<T: SurfaceTypeTrait>;

    /// Returns `true` if this context is the current one on this thread.
    fn is_current(&self) -> bool;

    /// Make the context not current to the current thread.
    fn make_not_current(self) -> Result<Self::NotCurrentContext>;

    /// Make the context not current to the current thread, in place.
    fn make_not_current_in_place(&self) -> Result<()>;

    /// Make the surface current on the calling thread.
    fn make_current<T: SurfaceTypeTrait>(&self, surface: &Self::Surface<T>) -> Result<()>;
}

/// The builder to help customize the context.
#[derive(Default, Debug, Clone)]
pub struct ContextAttributesBuilder {
    attributes: ContextAttributes,
}

impl ContextAttributesBuilder {
    /// Create a new builder.
    pub fn new() -> Self {
        Default::default()
    }

    /// Sets the *debug* flag for the OpenGL context.
    pub fn with_debug(mut self, debug: bool) -> Self {
        self.attributes.debug = debug;
        self
    }

    /// Sets the robustness of the OpenGL context.
    pub fn with_robustness(mut self, robustness: Robustness) -> Self {
        self.attributes.robustness = robustness;
        self
    }

    /// Set the desired OpenGL context profile.
    pub fn with_profile(mut self, profile: GlProfile) -> Self {
        self.attributes.profile = Some(profile);
        self
    }

    /// Set the desired OpenGL context api.
    pub fn with_context_api(mut self, api: ContextApi) -> Self {
        self.attributes.api = Some(api);
        self
    }

    /// Build the context attributes.
    pub fn build(mut self, raw_window_handle: Option<RawWindowHandle>) -> ContextAttributes {
        self.attributes.raw_window_handle = raw_window_handle;
        self.attributes
    }
}

/// The attributes that are used to create a graphics context. gl9egl ignores
/// them all (one context flavor: desktop OpenGL over OSMesa); carried for
/// signature fidelity.
#[derive(Default, Debug, Clone)]
pub struct ContextAttributes {
    pub(crate) debug: bool,
    pub(crate) robustness: Robustness,
    pub(crate) profile: Option<GlProfile>,
    pub(crate) api: Option<ContextApi>,
    pub(crate) raw_window_handle: Option<RawWindowHandle>,
}

/// Specifies the tolerance of the OpenGL context to faults.
#[derive(Debug, Copy, Clone, PartialEq, Eq, Default)]
pub enum Robustness {
    /// Not everything is checked.
    #[default]
    NotRobust,

    /// The driver doesn't check anything.
    NoError,

    /// Everything is checked, no crash guarantee, no reset notification.
    RobustNoResetNotification,

    /// Everything is checked, context enters "lost" state on problem.
    RobustLoseContextOnReset,
}

/// Describes the requested OpenGL context profiles.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GlProfile {
    /// Include all the future-compatible functions and definitions.
    Core,
    /// Include all the immediate mode functions and definitions.
    Compatibility,
}

/// The rendering Api the context should support.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ContextApi {
    /// OpenGL Api version that should be used by the context.
    OpenGl(Option<Version>),

    /// OpenGL ES Api version that should be used by the context.
    Gles(Option<Version>),
}

impl Default for ContextApi {
    fn default() -> Self {
        Self::OpenGl(None)
    }
}

/// The version used to index the Api.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct Version {
    /// Major version of the Api.
    pub major: u8,
    /// Minor version of the Api.
    pub minor: u8,
}

impl Version {
    /// Create a new version with the given `major` and `minor` values.
    pub const fn new(major: u8, minor: u8) -> Self {
        Self { major, minor }
    }
}

/// A context that is known to be not current on the current thread.
#[derive(Debug)]
pub enum NotCurrentContext {
    /// The EGL context.
    Egl(NotCurrentEglContext),
}

/// A context that is possibly current on the current thread.
#[derive(Debug)]
pub enum PossiblyCurrentContext {
    /// The EGL context.
    Egl(PossiblyCurrentEglContext),
}

fn egl_make_current(display: usize, surface: usize, context: usize) -> Result<()> {
    let ok = unsafe {
        ffi::eglMakeCurrent(
            display as ffi::EGLDisplay,
            surface as ffi::EGLSurface,
            surface as ffi::EGLSurface,
            context as ffi::EGLContext,
        )
    };
    if ok == ffi::EGL_FALSE {
        Err(last_egl_error())
    } else {
        egl_context::set_current(context);
        Ok(())
    }
}

impl NotCurrentGlContext for NotCurrentContext {
    type PossiblyCurrentContext = PossiblyCurrentContext;
    type Surface<T: SurfaceTypeTrait> = Surface<T>;

    fn treat_as_possibly_current(self) -> Self::PossiblyCurrentContext {
        let Self::Egl(context) = self;
        PossiblyCurrentContext::Egl(PossiblyCurrentEglContext {
            raw: context.raw,
            display: context.display,
            config: context.config,
        })
    }

    fn make_current<T: SurfaceTypeTrait>(
        self,
        surface: &Self::Surface<T>,
    ) -> Result<Self::PossiblyCurrentContext> {
        let (Self::Egl(context), Surface::Egl(surface)) = (self, surface);
        egl_make_current(context.display, surface.raw, context.raw)?;
        Ok(PossiblyCurrentContext::Egl(PossiblyCurrentEglContext {
            raw: context.raw,
            display: context.display,
            config: context.config,
        }))
    }
}

impl PossiblyCurrentGlContext for PossiblyCurrentContext {
    type NotCurrentContext = NotCurrentContext;
    type Surface<T: SurfaceTypeTrait> = Surface<T>;

    fn is_current(&self) -> bool {
        let Self::Egl(context) = self;
        egl_context::is_current(context.raw)
    }

    fn make_not_current(self) -> Result<Self::NotCurrentContext> {
        self.make_not_current_in_place()?;
        let Self::Egl(context) = self;
        Ok(NotCurrentContext::Egl(NotCurrentEglContext {
            raw: context.raw,
            display: context.display,
            config: context.config,
        }))
    }

    fn make_not_current_in_place(&self) -> Result<()> {
        let Self::Egl(context) = self;
        egl_make_current(context.display, 0, 0)?;
        egl_context::set_current(0);
        Ok(())
    }

    fn make_current<T: SurfaceTypeTrait>(&self, surface: &Self::Surface<T>) -> Result<()> {
        let (Self::Egl(context), Surface::Egl(surface)) = (self, surface);
        egl_make_current(context.display, surface.raw, context.raw)
    }
}

impl GlContext for NotCurrentContext {
    fn context_api(&self) -> ContextApi {
        // gl9egl only creates desktop OpenGL (OSMesa) contexts, which is what
        // alacritty's glsl3 renderer path expects.
        ContextApi::OpenGl(None)
    }
}

impl GlContext for PossiblyCurrentContext {
    fn context_api(&self) -> ContextApi {
        ContextApi::OpenGl(None)
    }
}

impl GetGlConfig for NotCurrentContext {
    type Target = Config;

    fn config(&self) -> Self::Target {
        let Self::Egl(context) = self;
        Config::Egl(crate::api::egl::config::Config {
            raw: context.config,
            display: context.display,
        })
    }
}

impl GetGlConfig for PossiblyCurrentContext {
    type Target = Config;

    fn config(&self) -> Self::Target {
        let Self::Egl(context) = self;
        Config::Egl(crate::api::egl::config::Config {
            raw: context.config,
            display: context.display,
        })
    }
}

impl GetGlDisplay for NotCurrentContext {
    type Target = Display;

    fn display(&self) -> Self::Target {
        let Self::Egl(context) = self;
        Display::Egl(crate::api::egl::display::Display { raw: context.display })
    }
}

impl GetGlDisplay for PossiblyCurrentContext {
    type Target = Display;

    fn display(&self) -> Self::Target {
        let Self::Egl(context) = self;
        Display::Egl(crate::api::egl::display::Display { raw: context.display })
    }
}
