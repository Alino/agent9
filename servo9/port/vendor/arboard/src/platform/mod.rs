// Plan 9's clipboard is /dev/snarf — a file. No X11/Wayland selection protocol,
// so the `linux` backend (which is all x11rb/wayland) does not apply.
#[cfg(target_os = "plan9")]
mod plan9;
#[cfg(target_os = "plan9")]
pub use plan9::*;

#[cfg(all(unix,
    not(target_os = "plan9"), not(any(target_os = "macos", target_os = "android", target_os = "emscripten", target_os = "plan9"))))]
mod linux;
#[cfg(all(
	unix,
	not(any(target_os = "macos", target_os = "android", target_os = "emscripten", target_os = "plan9"))
))]
pub use linux::*;

#[cfg(windows)]
mod windows;
#[cfg(windows)]
pub use windows::*;

#[cfg(target_os = "macos")]
mod osx;
#[cfg(target_os = "macos")]
pub use osx::*;
