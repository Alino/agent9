//! P3 gate: drive the exact stack Alacritty will use — winit plan9 event
//! loop + glutin shim + gl9egl + Mesa softpipe — with the smallest possible
//! GL payload (clear to a color, keys recolor, resize re-sizes the surface).
//! Frame timings go to stderr so the gate can record swap latency.

use std::ffi::CString;
use std::num::NonZeroU32;
use std::time::Instant;

use glutin::config::ConfigTemplateBuilder;
use glutin::context::{ContextAttributesBuilder, PossiblyCurrentContext};
use glutin::display::{Display, DisplayApiPreference};
use glutin::prelude::*;
use glutin::surface::{Surface, SurfaceAttributesBuilder, WindowSurface};
use winit::application::ApplicationHandler;
use winit::event::{ElementState, KeyEvent, WindowEvent};
use winit::event_loop::{ActiveEventLoop, EventLoop};
use winit::keyboard::{Key, NamedKey};
use winit::raw_window_handle::{HasDisplayHandle, HasWindowHandle};
use winit::window::{Window, WindowId};

const GL_COLOR_BUFFER_BIT: u32 = 0x4000;

type GlClearColor = unsafe extern "C" fn(f32, f32, f32, f32);
type GlClear = unsafe extern "C" fn(u32);

#[derive(Default)]
struct App {
    window: Option<Window>,
    context: Option<PossiblyCurrentContext>,
    surface: Option<Surface<WindowSurface>>,
    gl_clear_color: Option<GlClearColor>,
    gl_clear: Option<GlClear>,
    color: (f32, f32, f32),
    frames: u32,
}

impl App {
    fn redraw(&mut self) {
        let (Some(surface), Some(context)) = (&self.surface, &self.context) else {
            return;
        };
        let t = Instant::now();
        unsafe {
            (self.gl_clear_color.unwrap())(self.color.0, self.color.1, self.color.2, 1.0);
            (self.gl_clear.unwrap())(GL_COLOR_BUFFER_BIT);
        }
        surface.swap_buffers(context).expect("swap_buffers");
        eprintln!("glclear: frame {} rendered+swapped in {:?}", self.frames, t.elapsed());
        self.frames += 1;
    }
}

impl ApplicationHandler for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.window.is_some() {
            return;
        }

        let window =
            event_loop.create_window(Window::default_attributes()).expect("create window");
        let raw_display = window.display_handle().expect("display handle").as_raw();
        let raw_window = window.window_handle().expect("window handle").as_raw();

        let display =
            unsafe { Display::new(raw_display, DisplayApiPreference::Egl) }.expect("display");
        eprintln!("glclear: EGL {}", display.version_string());

        let config = unsafe { display.find_configs(ConfigTemplateBuilder::new().build()) }
            .expect("find_configs")
            .next()
            .expect("no config");

        let attrs = ContextAttributesBuilder::new().build(Some(raw_window));
        let not_current = unsafe { display.create_context(&config, &attrs) }.expect("context");

        let size = window.inner_size();
        let surf_attrs = SurfaceAttributesBuilder::<WindowSurface>::new().build(
            raw_window,
            NonZeroU32::new(size.width.max(1)).unwrap(),
            NonZeroU32::new(size.height.max(1)).unwrap(),
        );
        let surface =
            unsafe { display.create_window_surface(&config, &surf_attrs) }.expect("surface");
        let context = not_current.make_current(&surface).expect("make current");

        let load = |name: &str| display.get_proc_address(&CString::new(name).unwrap());
        let clear_color = load("glClearColor");
        let clear = load("glClear");
        assert!(!clear_color.is_null() && !clear.is_null(), "GL functions not found");
        self.gl_clear_color = Some(unsafe { std::mem::transmute(clear_color) });
        self.gl_clear = Some(unsafe { std::mem::transmute(clear) });

        self.color = (0.2, 0.2, 0.2);
        self.context = Some(context);
        self.surface = Some(surface);
        window.request_redraw();
        self.window = Some(window);
        eprintln!("glclear: GL ready, {}x{}", size.width, size.height);
    }

    fn window_event(&mut self, event_loop: &ActiveEventLoop, _id: WindowId, event: WindowEvent) {
        match event {
            WindowEvent::CloseRequested => event_loop.exit(),
            WindowEvent::Resized(size) => {
                eprintln!("glclear: resized {}x{}", size.width, size.height);
                if let (Some(surface), Some(context)) = (&self.surface, &self.context) {
                    surface.resize(
                        context,
                        NonZeroU32::new(size.width.max(1)).unwrap(),
                        NonZeroU32::new(size.height.max(1)).unwrap(),
                    );
                }
                if let Some(window) = &self.window {
                    window.request_redraw();
                }
            },
            WindowEvent::KeyboardInput {
                event: KeyEvent { logical_key, state: ElementState::Pressed, .. },
                ..
            } => {
                match logical_key.as_ref() {
                    Key::Character("r") => self.color = (1.0, 0.0, 0.0),
                    Key::Character("g") => self.color = (0.0, 1.0, 0.0),
                    Key::Character("b") => self.color = (0.0, 0.0, 1.0),
                    Key::Named(NamedKey::Escape) => event_loop.exit(),
                    _ => (),
                }
                if let Some(window) = &self.window {
                    window.request_redraw();
                }
            },
            WindowEvent::RedrawRequested => self.redraw(),
            _ => (),
        }
    }
}

fn main() {
    let event_loop = EventLoop::new().expect("event loop");
    let mut app = App::default();
    event_loop.run_app(&mut app).expect("run");
    eprintln!("glclear: exiting after {} frames", app.frames);
}
