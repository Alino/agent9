//! Plan 9 (9front / cc9 runtime) platform abstraction layer.
//! Startup handoff: cc9's crt0 owns `_start` and calls rustc's `main` shim,
//! which calls `lang_start` -> `sys::init` (here). We stash argc/argv for
//! `std::env::args`.
#![allow(unsafe_op_in_unsafe_fn)]

pub mod sync;

use crate::io;

// SAFETY: must be called only once during runtime initialization.
pub unsafe fn init(argc: isize, argv: *const *const u8, _sigpipe: u8) {
    crate::sys::args::init(argc, argv);
}

// SAFETY: must be called only once during runtime cleanup.
pub unsafe fn cleanup() {}

pub fn unsupported<T>() -> io::Result<T> {
    Err(unsupported_err())
}

pub fn unsupported_err() -> io::Error {
    io::Error::UNSUPPORTED_PLATFORM
}

pub fn abort_internal() -> ! {
    core::intrinsics::abort();
}
