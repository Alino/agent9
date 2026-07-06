//! Compile-time mirror of every glutin call shape in alacritty/src.
//! Each block is copied call-shape by call-shape from the named file so
//! `cargo +nightly check --examples` (plan9 target) proves the shim's API
//! surface before alacritty itself can be built. Never meant to run.

use std::ffi::CString;
use std::mem::ManuallyDrop;
use std::num::NonZeroU32;
use std::ops::Deref;

use glutin::config::{ColorBufferType, Config, ConfigTemplateBuilder, GetGlConfig};
use glutin::context::{
    ContextApi, ContextAttributesBuilder, GlProfile, NotCurrentContext, PossiblyCurrentContext,
    Robustness, Version,
};
use glutin::display::{Display, DisplayApiPreference, DisplayFeatures, GetGlDisplay};
use glutin::error::{ErrorKind, Result as GlutinResult};
use glutin::prelude::*;
use glutin::surface::{Rect, Surface, SurfaceAttributesBuilder, SwapInterval, WindowSurface};
use raw_window_handle::{
    RawDisplayHandle, RawWindowHandle, WebDisplayHandle, WebWindowHandle,
};

// --- renderer/platform.rs: create_gl_display ---
fn create_gl_display(
    raw_display_handle: RawDisplayHandle,
    _raw_window_handle: Option<RawWindowHandle>,
    _prefer_egl: bool,
) -> GlutinResult<Display> {
    let preference = DisplayApiPreference::Egl;
    let display = unsafe { Display::new(raw_display_handle, preference)? };
    println!("Using {}", { display.version_string() });
    Ok(display)
}

// --- renderer/platform.rs: pick_gl_config ---
fn pick_gl_config(
    gl_display: &Display,
    raw_window_handle: Option<RawWindowHandle>,
) -> Result<Config, String> {
    let mut default_config = ConfigTemplateBuilder::new()
        .with_depth_size(0)
        .with_stencil_size(0)
        .with_transparency(true);

    if let Some(raw_window_handle) = raw_window_handle {
        default_config = default_config.compatible_with_native_window(raw_window_handle);
    }

    let config_10bit = default_config
        .clone()
        .with_buffer_type(ColorBufferType::Rgb { r_size: 10, g_size: 10, b_size: 10 })
        .with_alpha_size(2);

    let configs = [
        default_config.clone(),
        config_10bit.clone(),
        default_config.with_transparency(false),
        config_10bit.with_transparency(false),
    ];

    for config in configs {
        let gl_config = unsafe {
            gl_display.find_configs(config.build()).ok().and_then(|mut configs| configs.next())
        };

        if let Some(gl_config) = gl_config {
            println!(
                "{:?} {} {} {:?} {:?} {:?} {}",
                gl_config.color_buffer_type(),
                gl_config.alpha_size(),
                gl_config.num_samples(),
                gl_config.hardware_accelerated(),
                gl_config.supports_transparency(),
                gl_config.api(),
                gl_config.srgb_capable(),
            );

            return Ok(gl_config);
        }
    }

    Err(String::from("failed to find suitable GL configuration."))
}

// --- renderer/platform.rs: create_gl_context ---
fn create_gl_context(
    gl_display: &Display,
    gl_config: &Config,
    raw_window_handle: Option<RawWindowHandle>,
) -> GlutinResult<NotCurrentContext> {
    let debug = false;

    let apis = [
        (ContextApi::OpenGl(Some(Version::new(3, 3))), GlProfile::Core),
        (ContextApi::Gles(Some(Version::new(2, 0))), GlProfile::Core),
        (ContextApi::OpenGl(Some(Version::new(2, 1))), GlProfile::Compatibility),
    ];

    let robustness = gl_display.supported_features().contains(DisplayFeatures::CONTEXT_ROBUSTNESS);
    let robustness: &[Robustness] = if robustness {
        &[Robustness::RobustLoseContextOnReset, Robustness::NotRobust]
    } else {
        &[Robustness::NotRobust]
    };

    let mut error = None;
    for (api, profile) in apis {
        for robustness in robustness {
            let attributes = ContextAttributesBuilder::new()
                .with_debug(debug)
                .with_context_api(api)
                .with_profile(profile)
                .with_robustness(*robustness)
                .build(raw_window_handle);

            match unsafe { gl_display.create_context(gl_config, &attributes) } {
                Ok(profile) => return Ok(profile),
                Err(err) => error = Some(err),
            }
        }
    }

    Err(error.unwrap())
}

// --- renderer/platform.rs: create_gl_surface ---
fn create_gl_surface(
    gl_context: &NotCurrentContext,
    size: (u32, u32),
    raw_window_handle: RawWindowHandle,
) -> GlutinResult<Surface<WindowSurface>> {
    let gl_display = gl_context.display();
    let gl_config = gl_context.config();

    let surface_attributes =
        SurfaceAttributesBuilder::<WindowSurface>::new().with_srgb(Some(false)).build(
            raw_window_handle,
            NonZeroU32::new(size.0).unwrap(),
            NonZeroU32::new(size.1).unwrap(),
        );

    unsafe { gl_display.create_window_surface(&gl_config, &surface_attributes) }
}

// --- display/mod.rs: Error wrapping (From<glutin::error::Error>) ---
#[derive(Debug)]
enum DisplayError {
    Context(glutin::error::Error),
}

impl std::error::Error for DisplayError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            DisplayError::Context(err) => err.source(),
        }
    }
}

impl std::fmt::Display for DisplayError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            DisplayError::Context(err) => err.fmt(f),
        }
    }
}

impl From<glutin::error::Error> for DisplayError {
    fn from(val: glutin::error::Error) -> Self {
        DisplayError::Context(val)
    }
}

// --- display/mod.rs: the Display struct's context/surface handling ---
struct TermDisplay {
    surface: ManuallyDrop<Surface<WindowSurface>>,
    context: ManuallyDrop<PossiblyCurrentContext>,
    raw_window_handle: RawWindowHandle,
    debug_damage: bool,
}

impl TermDisplay {
    // display/mod.rs:436 + 481 + 513
    fn new(
        gl_context: NotCurrentContext,
        surface: Surface<WindowSurface>,
    ) -> Result<Self, DisplayError> {
        // Make the context current.
        let context = gl_context.make_current(&surface)?;

        surface.swap_buffers(&context).expect("failed to swap buffers.");

        if let Err(err) = surface.set_swap_interval(&context, SwapInterval::DontWait) {
            println!("Failed to set swap interval: {err}");
        }

        Ok(Self {
            surface: ManuallyDrop::new(surface),
            context: ManuallyDrop::new(context),
            raw_window_handle: RawWindowHandle::Web(WebWindowHandle::new(1)),
            debug_damage: false,
        })
    }

    // display/mod.rs:550
    fn make_not_current(&mut self) {
        if self.context.is_current() {
            self.context.make_not_current_in_place().expect("failed to disable context");
        }
    }

    // display/mod.rs:556 (context loss/recreation path)
    fn make_current(&mut self) {
        let is_current = self.context.is_current();

        let context_loss = if is_current {
            false
        } else {
            match self.context.make_current(&self.surface) {
                Err(err) if err.error_kind() == ErrorKind::ContextLost => true,
                _ => false,
            }
        };

        if !context_loss {
            return;
        }

        let gl_display = self.context.display();
        let gl_config = self.context.config();
        let raw_window_handle = Some(self.raw_window_handle);
        let context = create_gl_context(&gl_display, &gl_config, raw_window_handle)
            .expect("failed to recreate context.");

        unsafe {
            ManuallyDrop::drop(&mut self.context);
        }

        let context = context.treat_as_possibly_current();
        self.context = ManuallyDrop::new(context);
        self.context.make_current(&self.surface).expect("failed to reativate context after reset.");
    }

    // display/mod.rs:607 (the exact damage/plain swap match)
    fn swap_buffers(&self) {
        #[allow(clippy::single_match)]
        let res = match (self.surface.deref(), &self.context.deref()) {
            (Surface::Egl(surface), PossiblyCurrentContext::Egl(context))
                if matches!(self.raw_window_handle, RawWindowHandle::Wayland(_))
                    && !self.debug_damage =>
            {
                let damage = shape_frame_damage();
                surface.swap_buffers_with_damage(context, &damage)
            },
            (surface, context) => surface.swap_buffers(context),
        };
        if let Err(err) = res {
            println!("error calling swap_buffers: {err}");
        }
    }

    // display/mod.rs:754
    fn resize(&self) {
        let width = NonZeroU32::new(640).unwrap();
        let height = NonZeroU32::new(480).unwrap();
        self.surface.resize(&self.context, width, height);
    }
}

// --- display/damage.rs: Rect construction (both shapes) ---
fn shape_frame_damage() -> Vec<Rect> {
    let mut rects = vec![Rect::new(0, 0, 640, 480)];
    rects.push(Rect { x: 1, y: 2, width: 3, height: 4 });
    let _overlap = rects[0].x + rects[0].y + rects[0].width + rects[0].height;
    rects
}

// --- renderer/mod.rs: Renderer::new ---
fn renderer_new(context: &PossiblyCurrentContext) -> bool {
    let gl_display = context.display();
    let symbol = CString::new("glClear").unwrap();
    let _addr: *const std::ffi::c_void = gl_display.get_proc_address(symbol.as_c_str()).cast();

    // renderer/mod.rs:143
    let is_gles_context = matches!(context.context_api(), ContextApi::Gles(_));
    is_gles_context
}

// --- window_context.rs:163 ---
fn store_gl_config(gl_context: &PossiblyCurrentContext) -> Option<Config> {
    Some(gl_context.config())
}

// --- event.rs:497 (exiting: terminate the EGL display) ---
fn exiting(mut gl_config: Option<Config>) {
    match gl_config.take().map(|config| config.display()) {
        Some(glutin::display::Display::Egl(display)) => {
            unsafe {
                display.terminate();
            }
        },
        _ => (),
    }
}

fn main() {
    let raw_display_handle = RawDisplayHandle::Web(WebDisplayHandle::new());
    let raw_window_handle = RawWindowHandle::Web(WebWindowHandle::new(1));

    let gl_display = create_gl_display(raw_display_handle, Some(raw_window_handle), true).unwrap();
    let gl_config = pick_gl_config(&gl_display, Some(raw_window_handle)).unwrap();
    let gl_context = create_gl_context(&gl_display, &gl_config, Some(raw_window_handle)).unwrap();
    let surface = create_gl_surface(&gl_context, (640, 480), raw_window_handle).unwrap();

    let mut term_display = TermDisplay::new(gl_context, surface).unwrap();
    term_display.make_current();
    term_display.swap_buffers();
    term_display.resize();
    term_display.make_not_current();

    let _ = renderer_new(&term_display.context);
    let gl_config = store_gl_config(&term_display.context);
    exiting(gl_config);
}
