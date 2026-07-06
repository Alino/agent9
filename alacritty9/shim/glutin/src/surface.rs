//! A cross platform OpenGL surface representation. Signature-mirrors real
//! glutin 0.32.3 `surface.rs` for the surface alacritty touches.

use std::marker::PhantomData;
use std::num::NonZeroU32;

use raw_window_handle::RawWindowHandle;

use crate::api::egl::ffi;
use crate::api::egl::surface::Surface as EglSurface;
use crate::context::PossiblyCurrentContext;
use crate::display::{Display, GetGlDisplay};
use crate::error::Result;

/// A trait to group common operations on the surface.
pub trait GlSurface<T: SurfaceTypeTrait> {
    /// The context to access surface data.
    type Context;

    /// Swaps the underlying back buffers.
    fn swap_buffers(&self, context: &Self::Context) -> Result<()>;

    /// Set the swap interval for the surface.
    fn set_swap_interval(&self, context: &Self::Context, interval: SwapInterval) -> Result<()>;

    /// Resize the surface to a new size.
    fn resize(&self, context: &Self::Context, width: NonZeroU32, height: NonZeroU32);
}

/// The marker trait to indicate the type of the surface.
pub trait SurfaceTypeTrait {}

/// Builder to get the required set of attributes initialized before hand.
#[derive(Default, Debug, Clone)]
pub struct SurfaceAttributesBuilder<T: SurfaceTypeTrait + Default> {
    attributes: SurfaceAttributes<T>,
}

impl<T: SurfaceTypeTrait + Default> SurfaceAttributesBuilder<T> {
    /// Get new surface attributes.
    pub fn new() -> Self {
        Default::default()
    }

    /// Specify whether the surface should support srgb or not. Ignored by
    /// gl9egl.
    pub fn with_srgb(mut self, srgb: Option<bool>) -> Self {
        self.attributes.srgb = srgb;
        self
    }
}

impl SurfaceAttributesBuilder<WindowSurface> {
    /// Build the surface attributes suitable to create a window surface.
    ///
    /// The raw window handle is ignored on Plan 9 — the surface size is taken
    /// from `width`/`height` (which is exactly how alacritty calls this).
    pub fn build(
        mut self,
        raw_window_handle: RawWindowHandle,
        width: NonZeroU32,
        height: NonZeroU32,
    ) -> SurfaceAttributes<WindowSurface> {
        self.attributes.raw_window_handle = Some(raw_window_handle);
        self.attributes.width = Some(width);
        self.attributes.height = Some(height);
        self.attributes
    }
}

/// Attributes which are used for creating a particular surface.
#[derive(Default, Debug, Clone)]
pub struct SurfaceAttributes<T: SurfaceTypeTrait> {
    pub(crate) srgb: Option<bool>,
    pub(crate) width: Option<NonZeroU32>,
    pub(crate) height: Option<NonZeroU32>,
    pub(crate) raw_window_handle: Option<RawWindowHandle>,
    _ty: PhantomData<T>,
}

/// Marker used to type-gate methods for window surfaces.
#[derive(Default, Debug, Clone, Copy, PartialEq, Eq)]
pub struct WindowSurface;

impl SurfaceTypeTrait for WindowSurface {}

/// Marker used to type-gate methods for pbuffer surfaces.
#[derive(Default, Debug, Clone, Copy, PartialEq, Eq)]
pub struct PbufferSurface;

impl SurfaceTypeTrait for PbufferSurface {}

/// The GL surface that is used for rendering.
#[derive(Debug)]
pub enum Surface<T: SurfaceTypeTrait> {
    /// The EGL surface.
    Egl(EglSurface<T>),
}

impl<T: SurfaceTypeTrait> GlSurface<T> for Surface<T> {
    type Context = PossiblyCurrentContext;

    fn swap_buffers(&self, context: &Self::Context) -> Result<()> {
        let (Self::Egl(surface), PossiblyCurrentContext::Egl(context)) = (self, context);
        surface.swap_buffers(context)
    }

    fn set_swap_interval(&self, _context: &Self::Context, interval: SwapInterval) -> Result<()> {
        let Self::Egl(surface) = self;
        let interval = match interval {
            SwapInterval::DontWait => 0,
            SwapInterval::Wait(n) => n.get() as i32,
        };
        // A no-op in gl9egl, but keep the call honest.
        unsafe {
            ffi::eglSwapInterval(surface.display as ffi::EGLDisplay, interval);
        }
        Ok(())
    }

    fn resize(&self, _context: &Self::Context, width: NonZeroU32, height: NonZeroU32) {
        let Self::Egl(surface) = self;
        unsafe {
            ffi::gl9egl_surface_resize(
                surface.raw as ffi::EGLSurface,
                width.get() as i32,
                height.get() as i32,
            );
        }
    }
}

impl<T: SurfaceTypeTrait> GetGlDisplay for Surface<T> {
    type Target = Display;

    fn display(&self) -> Self::Target {
        let Self::Egl(surface) = self;
        Display::Egl(crate::api::egl::display::Display { raw: surface.display })
    }
}

/// A swap interval.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SwapInterval {
    /// `swap_buffers` will not block.
    DontWait,

    /// The swap is synchronized to the `n`'th video frame.
    Wait(NonZeroU32),
}

/// The rect that is being used in various surface operations.
///
/// The origin is in the bottom left of the surface.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Hash)]
pub struct Rect {
    /// `X` of the origin.
    pub x: i32,
    /// `Y` of the origin.
    pub y: i32,
    /// Rect width.
    pub width: i32,
    /// Rect height.
    pub height: i32,
}

impl Rect {
    /// Helper to simplify rectangle creation.
    pub fn new(x: i32, y: i32, width: i32, height: i32) -> Self {
        Self { x, y, width, height }
    }
}
