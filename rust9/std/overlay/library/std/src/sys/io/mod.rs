#![forbid(unsafe_op_in_unsafe_fn)]

mod error;

mod is_terminal {
    cfg_select! {
        any(target_family = "unix", target_os = "wasi") => {
            mod isatty;
            pub use isatty::*;
        }
        target_os = "windows" => {
            mod windows;
            pub use windows::*;
        }
        target_os = "hermit" => {
            mod hermit;
            pub use hermit::*;
        }
        target_os = "motor" => {
            mod motor;
            pub use motor::*;
        }
        // plan9 (cc9): not target_family="unix", so it would fall to `unsupported`
        // (always false) and colors/prompts/progress bars would never engage on a
        // real console. cc9's isatty is correct (fd 0-2 && $TERM), and File/Stdin/…
        // all impl AsRawFd here now, so route through it like the unix arm.
        target_os = "plan9" => {
            mod plan9;
            pub use plan9::*;
        }
        _ => {
            mod unsupported;
            pub use unsupported::*;
        }
    }
}

mod kernel_copy;

#[cfg_attr(not(target_os = "linux"), allow(unused_imports))]
#[cfg(all(
    target_family = "unix",
    not(any(target_os = "dragonfly", target_os = "vxworks", target_os = "rtems"))
))]
pub use error::errno_location;
#[cfg_attr(not(target_os = "linux"), allow(unused_imports))]
#[cfg(any(
    all(target_family = "unix", not(any(target_os = "vxworks", target_os = "rtems"))),
    target_os = "wasi",
))]
pub use error::set_errno;
pub use error::{decode_error_kind, errno, error_string, is_interrupted};
pub use is_terminal::is_terminal;
pub use kernel_copy::{CopyState, kernel_copy};

// Bare metal platforms usually have very small amounts of RAM
// (in the order of hundreds of KB)
pub const DEFAULT_BUF_SIZE: usize = if cfg!(target_os = "espidf") { 512 } else { 8 * 1024 };
