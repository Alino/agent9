#![no_std]
//! First Rust program to run on 9front. Compiled for x86_64-unknown-none as a
//! staticlib, linked against the cc9 runtime (crt0 `_start` -> our `main`).
use core::panic::PanicInfo;

extern "C" {
    fn n9_pwrite(fd: i32, buf: *const u8, n: isize, off: i64) -> isize;
    fn n9_exits(msg: *const u8);
}

#[no_mangle]
pub extern "C" fn main(_argc: i32, _argv: *const *const u8) -> i32 {
    // Prove real Rust: build the string with core (slice, iterator) not a bare literal.
    let msg = b"hi from Rust on 9front\n";
    unsafe {
        n9_pwrite(1, msg.as_ptr(), msg.len() as isize, -1);
    }
    0
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    unsafe { n9_exits(b"rust: panic\0".as_ptr()); }
    loop {}
}
