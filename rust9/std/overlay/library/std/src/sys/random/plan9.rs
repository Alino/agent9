//! Plan 9 randomness: read from /dev/random.
//!
//! Failure aborts rather than returning: callers (HashMap seeds, key material)
//! assume the buffer is filled, and silently-zeroed "randomness" is a security
//! bug. /dev/random exists on every 9front kernel, so this never fires in
//! practice — only in a broken namespace.
unsafe extern "C" {
    fn open(path: *const u8, oflag: i32, ...) -> i32;
    fn read(fd: i32, buf: *mut u8, n: usize) -> isize;
    fn close(fd: i32) -> i32;
}

pub fn fill_bytes(bytes: &mut [u8]) {
    // O_RDONLY == 0
    let fd = unsafe { open(c"/dev/random".as_ptr() as *const u8, 0) };
    if fd < 0 {
        panic!("failed to open /dev/random");
    }
    let mut filled = 0;
    while filled < bytes.len() {
        let n = unsafe { read(fd, bytes[filled..].as_mut_ptr(), bytes.len() - filled) };
        if n <= 0 {
            unsafe { close(fd) };
            panic!("failed to read /dev/random");
        }
        filled += n as usize;
    }
    unsafe { close(fd) };
}
