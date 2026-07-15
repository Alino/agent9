//! The `getrandom` custom backend for plan9, over 9front's /dev/random.
//!
//! Enabled by `--cfg getrandom_backend="custom"` (see servo9/host/plan9-env.sh).
//! getrandom 0.3 and 0.4 share the same hook symbol, `__getrandom_v03_custom`.
//!
//! Randomness is a security boundary — this is what seeds HashMap DoS protection
//! and TLS. So it reads /dev/random and reports failure honestly rather than
//! falling back to anything weaker: a short read is an error, never zero-filled
//! or padded with a clock value.

use core::ffi::c_int;

unsafe extern "C" {
    fn open(path: *const u8, flags: c_int, ...) -> c_int;
    fn read(fd: c_int, buf: *mut u8, n: usize) -> isize;
    fn close(fd: c_int) -> c_int;
}

/// Fill `dest` from /dev/random. Returns false on any short read or error.
unsafe fn devrandom(dest: *mut u8, len: usize) -> bool {
    if len == 0 {
        return true;
    }
    let fd = unsafe { open(b"/dev/random\0".as_ptr(), 0) };
    if fd < 0 {
        return false;
    }
    let mut off = 0usize;
    while off < len {
        // /dev/random can return short reads; loop rather than assume.
        let n = unsafe { read(fd, dest.add(off), len - off) };
        if n <= 0 {
            unsafe { close(fd) };
            return false;
        }
        off += n as usize;
    }
    unsafe { close(fd) };
    true
}

#[no_mangle]
unsafe extern "Rust" fn __getrandom_v03_custom(
    dest: *mut u8,
    len: usize,
) -> Result<(), getrandom::Error> {
    if unsafe { devrandom(dest, len) } {
        Ok(())
    } else {
        Err(getrandom::Error::UNEXPECTED)
    }
}
