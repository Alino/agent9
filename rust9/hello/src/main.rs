#![no_std]
#![no_main]
//! M2 proof: core+alloc built through cargo `-Zbuild-std` for the custom
//! x86_64-unknown-plan9 target (SSE on, hard-float ABI), linked by rust9-ld
//! against the cc9 runtime. Same output as the M1 hand-rolled staticlib.
extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::panic::PanicInfo;

extern "C" {
    fn posix_memalign(memptr: *mut *mut u8, align: usize, size: usize) -> i32;
    fn free(ptr: *mut u8);
    fn n9_pwrite(fd: i32, buf: *const u8, n: isize, off: i64) -> isize;
    fn n9_exits(msg: *const u8);
}

struct Cc9Alloc;
unsafe impl GlobalAlloc for Cc9Alloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let align = layout.align().max(core::mem::size_of::<usize>());
        let mut p: *mut u8 = core::ptr::null_mut();
        if posix_memalign(&mut p, align, layout.size()) != 0 {
            core::ptr::null_mut()
        } else {
            p
        }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        free(ptr);
    }
}

#[global_allocator]
static GLOBAL: Cc9Alloc = Cc9Alloc;

#[no_mangle]
pub extern "C" fn main(_argc: i32, _argv: *const *const u8) -> i32 {
    let squares: Vec<u64> = (1..=8u64).map(|n| n * n).collect();
    let sum: u64 = squares.iter().copied().sum();
    let s: String = format!(
        "Rust via cargo build-std on 9front: sum of squares 1..8 = {} (len {})\n",
        sum,
        squares.len()
    );
    unsafe {
        n9_pwrite(1, s.as_ptr(), s.len() as isize, -1);
    }
    0
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    unsafe { n9_exits(b"rust: panic\0".as_ptr()); }
    loop {}
}
