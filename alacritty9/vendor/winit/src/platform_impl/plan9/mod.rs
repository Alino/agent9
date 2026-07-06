#![cfg(target_os = "plan9")]

//! Plan 9 backend: the app is spawned by gl9win2 (which owns the rio window)
//! with events arriving as fixed records on stdin. See alacritty9/PROTOCOL.md.

use std::fmt::{self, Display, Formatter};
use std::sync::atomic::{AtomicU64, Ordering};

use smol_str::SmolStr;

use crate::dpi::{PhysicalPosition, PhysicalSize};
use crate::keyboard::{Key, NativeKeyCode, PhysicalKey};

pub(crate) use self::event_loop::{ActiveEventLoop, EventLoop, EventLoopProxy, OwnedDisplayHandle};
mod event_loop;

pub use self::window::Window;
mod window;

pub(crate) mod protocol;

// ponytail: exactly one window per process (gl9win2 owns it), so its last
// known size lives in a global; MonitorHandle/WindowId stay plain unit types.
static WINDOW_SIZE: AtomicU64 = AtomicU64::new((1024 << 32) | 768);

pub(crate) fn window_size() -> PhysicalSize<u32> {
    let packed = WINDOW_SIZE.load(Ordering::Relaxed);
    PhysicalSize::new((packed >> 32) as u32, packed as u32)
}

pub(crate) fn set_window_size(w: u32, h: u32) {
    WINDOW_SIZE.store(((w as u64) << 32) | h as u64, Ordering::Relaxed);
}

#[derive(Default, Debug, Copy, Clone, PartialEq, Eq, Hash)]
pub(crate) struct PlatformSpecificEventLoopAttributes {}

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct WindowId(u64);

impl WindowId {
    /// The one and only window.
    pub(crate) const PRIMARY: Self = WindowId(1);

    pub const fn dummy() -> Self {
        WindowId(u64::MAX)
    }
}

impl From<WindowId> for u64 {
    fn from(id: WindowId) -> Self {
        id.0
    }
}

impl From<u64> for WindowId {
    fn from(id: u64) -> Self {
        Self(id)
    }
}

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct DeviceId;

impl DeviceId {
    pub const fn dummy() -> Self {
        DeviceId
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct PlatformSpecificWindowAttributes;

#[derive(Clone, Debug)]
pub struct OsError(String);

impl OsError {
    #[allow(dead_code)]
    fn new(error: impl Display) -> Self {
        Self(error.to_string())
    }
}

impl Display for OsError {
    fn fmt(&self, fmt: &mut Formatter<'_>) -> Result<(), fmt::Error> {
        self.0.fmt(fmt)
    }
}

pub(crate) use crate::cursor::{
    NoCustomCursor as PlatformCustomCursor, NoCustomCursor as PlatformCustomCursorSource,
};
pub(crate) use crate::icon::NoIcon as PlatformIcon;

#[derive(Clone, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct MonitorHandle;

impl MonitorHandle {
    pub fn name(&self) -> Option<String> {
        Some("Plan 9".to_owned())
    }

    pub fn size(&self) -> PhysicalSize<u32> {
        // The rio window is all we can see; report its last known size.
        window_size()
    }

    pub fn position(&self) -> PhysicalPosition<i32> {
        (0, 0).into()
    }

    pub fn scale_factor(&self) -> f64 {
        1.0
    }

    pub fn refresh_rate_millihertz(&self) -> Option<u32> {
        None
    }

    pub fn video_modes(&self) -> impl Iterator<Item = VideoModeHandle> {
        let size = self.size().into();
        std::iter::once(VideoModeHandle {
            size,
            bit_depth: 32,
            refresh_rate_millihertz: 60000,
            monitor: self.clone(),
        })
    }
}

#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub struct VideoModeHandle {
    size: (u32, u32),
    bit_depth: u16,
    refresh_rate_millihertz: u32,
    monitor: MonitorHandle,
}

impl VideoModeHandle {
    pub fn size(&self) -> PhysicalSize<u32> {
        self.size.into()
    }

    pub fn bit_depth(&self) -> u16 {
        self.bit_depth
    }

    pub fn refresh_rate_millihertz(&self) -> u32 {
        self.refresh_rate_millihertz
    }

    pub fn monitor(&self) -> MonitorHandle {
        self.monitor.clone()
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Hash)]
pub struct KeyEventExtra {
    pub key_without_modifiers: Key,
    pub text_with_all_modifiers: Option<SmolStr>,
}

// There are no scancodes in the gl9win2 protocol; keys are identified by rune.
pub fn physicalkey_to_scancode(_key: PhysicalKey) -> Option<u32> {
    None
}

pub fn scancode_to_physicalkey(_scancode: u32) -> PhysicalKey {
    PhysicalKey::Unidentified(NativeKeyCode::Unidentified)
}
