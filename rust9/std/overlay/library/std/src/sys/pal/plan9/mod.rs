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

/// `cvt`: C return value -> io::Result. `os/fd/owned.rs` does `use crate::sys::cvt`
/// for every target outside its exclusion list, so plan9 needs it before
/// `std::os::fd` can exist. cc9's C functions use the POSIX convention (-1 plus
/// errno), like unix — not hermit's negated-errno return.
pub trait IsMinusOne {
    fn is_minus_one(&self) -> bool;
}

macro_rules! impl_is_minus_one {
    ($($t:ident)*) => ($(impl IsMinusOne for $t {
        fn is_minus_one(&self) -> bool {
            *self == -1
        }
    })*)
}

impl_is_minus_one! { i8 i16 i32 i64 isize }

pub fn cvt<T: IsMinusOne>(t: T) -> io::Result<T> {
    if t.is_minus_one() { Err(io::Error::last_os_error()) } else { Ok(t) }
}

pub fn cvt_r<T, F>(mut f: F) -> io::Result<T>
where
    T: IsMinusOne,
    F: FnMut() -> T,
{
    loop {
        match cvt(f()) {
            Err(ref e) if e.is_interrupted() => {}
            other => return other,
        }
    }
}
