//! Plan 9 argv: stashed at runtime init (cc9 crt0 passes argc/argv into `main`).
pub use super::common::Args;
use crate::ffi::{CStr, OsStr, c_char};
use crate::ptr;
use crate::sync::atomic::{AtomicIsize, AtomicPtr, Ordering};

static ARGC: AtomicIsize = AtomicIsize::new(0);
static ARGV: AtomicPtr<*const u8> = AtomicPtr::new(ptr::null_mut());

/// One-time global initialization, called from `sys::pal::plan9::init`.
pub unsafe fn init(argc: isize, argv: *const *const u8) {
    ARGC.store(argc, Ordering::Relaxed);
    ARGV.store(argv as *mut *const u8, Ordering::Relaxed);
}

pub fn args() -> Args {
    let argv = ARGV.load(Ordering::Relaxed);
    let argc = if argv.is_null() { 0 } else { ARGC.load(Ordering::Relaxed) };

    let mut vec = Vec::with_capacity(argc as usize);
    for i in 0..argc {
        // SAFETY: argv is non-null when argc > 0 and is at least argc long.
        let ptr = unsafe { (argv as *const *const u8).offset(i).read() };
        if ptr.is_null() {
            break;
        }
        let cstr = unsafe { CStr::from_ptr(ptr as *const c_char) };
        // plan9 OsStr is byte-based, so raw argv bytes are a valid encoding.
        vec.push(unsafe { OsStr::from_encoded_bytes_unchecked(cstr.to_bytes()).to_os_string() });
    }
    Args::new(vec)
}
