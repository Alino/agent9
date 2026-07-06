//! Api config picking and creating utils. Signature-mirrors real glutin
//! 0.32.3 `config.rs` for the surface alacritty touches; gl9egl has exactly
//! one config (RGBA8888/D24/S8), so the template is accepted and ignored.

use raw_window_handle::RawWindowHandle;

use crate::api::egl::config::Config as EglConfig;
use crate::display::{Display, GetGlDisplay};

/// The trait to group all common config options.
pub trait GlConfig {
    /// The type of the underlying color buffer.
    fn color_buffer_type(&self) -> Option<ColorBufferType>;

    /// The size of the alpha.
    fn alpha_size(&self) -> u8;

    /// The size of the depth buffer.
    fn depth_size(&self) -> u8;

    /// The size of the stencil buffer.
    fn stencil_size(&self) -> u8;

    /// The number of samples in the multisample buffer.
    fn num_samples(&self) -> u8;

    /// Whether the config supports creating srgb capable surfaces.
    fn srgb_capable(&self) -> bool;

    /// Whether the config supports creating transparent surfaces.
    fn supports_transparency(&self) -> Option<bool>;

    /// Whether the config is hardware accelerated.
    fn hardware_accelerated(&self) -> bool;

    /// The [`Api`] supported by the configuration.
    fn api(&self) -> Api;
}

/// Get the GL config used to create a particular GL object.
pub trait GetGlConfig {
    /// The config type.
    type Target: GlConfig;

    /// Get the GL config used to create a particular GL object.
    fn config(&self) -> Self::Target;
}

/// Builder for [`ConfigTemplate`].
#[derive(Debug, Default, Clone)]
pub struct ConfigTemplateBuilder {
    template: ConfigTemplate,
}

impl ConfigTemplateBuilder {
    /// Create a new configuration template builder.
    #[inline]
    pub fn new() -> Self {
        Default::default()
    }

    /// Number of alpha bits in the color buffer.
    #[inline]
    pub fn with_alpha_size(mut self, alpha_size: u8) -> Self {
        self.template.alpha_size = alpha_size;
        self
    }

    /// Number of bits in the stencil buffer.
    #[inline]
    pub fn with_stencil_size(mut self, stencil_size: u8) -> Self {
        self.template.stencil_size = stencil_size;
        self
    }

    /// Number of bits in the depth buffer.
    #[inline]
    pub fn with_depth_size(mut self, depth_size: u8) -> Self {
        self.template.depth_size = depth_size;
        self
    }

    /// The type of the color buffer.
    #[inline]
    pub fn with_buffer_type(mut self, color_buffer_type: ColorBufferType) -> Self {
        self.template.color_buffer_type = Some(color_buffer_type);
        self
    }

    /// Whether the configuration should support transparency.
    #[inline]
    pub fn with_transparency(mut self, transparency: bool) -> Self {
        self.template.transparency = transparency;
        self
    }

    /// Request a config that can render to a particular native window.
    pub fn compatible_with_native_window(mut self, native_window: RawWindowHandle) -> Self {
        self.template.native_window = Some(native_window);
        self
    }

    /// Build the template to match the configs against.
    #[must_use]
    pub fn build(self) -> ConfigTemplate {
        self.template
    }
}

/// The context configuration template used to find a desired config.
///
/// gl9egl has a single fixed config, so the fields are carried only for
/// signature fidelity and are ignored by `find_configs`.
#[derive(Debug, Default, Clone)]
pub struct ConfigTemplate {
    pub(crate) color_buffer_type: Option<ColorBufferType>,
    pub(crate) alpha_size: u8,
    pub(crate) depth_size: u8,
    pub(crate) stencil_size: u8,
    pub(crate) transparency: bool,
    pub(crate) native_window: Option<RawWindowHandle>,
}

/// The Api supported by the config. Bitflags-compatible surface without the
/// bitflags dependency (alacritty only debug-formats it).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Api(u8);

impl Api {
    /// Context supports OpenGL API.
    pub const OPENGL: Self = Self(0b0000_0001);
    /// Context supports OpenGL ES 1 API.
    pub const GLES1: Self = Self(0b0000_0010);
    /// Context supports OpenGL ES 2 API.
    pub const GLES2: Self = Self(0b0000_0100);
    /// Context supports OpenGL ES 3 API.
    pub const GLES3: Self = Self(0b0000_1000);

    /// Whether all bits in `other` are set.
    pub fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }
}

impl std::ops::BitOr for Api {
    type Output = Self;

    fn bitor(self, rhs: Self) -> Self {
        Self(self.0 | rhs.0)
    }
}

/// The buffer type backed by the config.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ColorBufferType {
    /// The backing buffer is using RGB format.
    Rgb {
        /// Size of the red component in bits.
        r_size: u8,
        /// Size of the green component in bits.
        g_size: u8,
        /// Size of the blue component in bits.
        b_size: u8,
    },

    /// The backing buffer is using Luminance.
    Luminance(u8),
}

/// The GL configuration used to create surfaces and contexts.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Config {
    /// The EGL config.
    Egl(EglConfig),
}

impl GlConfig for Config {
    // ponytail: gl9egl's one config is fixed RGBA8888/D24/S8 software
    // rendering — report the known constants instead of round-tripping
    // eglGetConfigAttrib.
    fn color_buffer_type(&self) -> Option<ColorBufferType> {
        Some(ColorBufferType::Rgb { r_size: 8, g_size: 8, b_size: 8 })
    }

    fn alpha_size(&self) -> u8 {
        8
    }

    fn depth_size(&self) -> u8 {
        24
    }

    fn stencil_size(&self) -> u8 {
        8
    }

    fn num_samples(&self) -> u8 {
        0
    }

    fn srgb_capable(&self) -> bool {
        false
    }

    fn supports_transparency(&self) -> Option<bool> {
        Some(true)
    }

    fn hardware_accelerated(&self) -> bool {
        false
    }

    fn api(&self) -> Api {
        Api::OPENGL | Api::GLES2
    }
}

impl GetGlDisplay for Config {
    type Target = Display;

    fn display(&self) -> Self::Target {
        let Self::Egl(config) = self;
        Display::Egl(crate::api::egl::display::Display { raw: config.display })
    }
}
