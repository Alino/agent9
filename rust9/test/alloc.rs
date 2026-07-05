#![no_std]
//! Prove the Rust heap on 9front: Vec/String/format! via a #[global_allocator]
//! backed by cc9's (thread-safe) posix_memalign/free.
extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use alloc::format;
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
        // posix_memalign wants align >= sizeof(void*) and a power of two.
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
    // realloc uses GlobalAlloc's default (alloc + copy + dealloc) — always correct.
}

#[global_allocator]
static GLOBAL: Cc9Alloc = Cc9Alloc;

#[no_mangle]
pub extern "C" fn main(_argc: i32, _argv: *const *const u8) -> i32 {
    // Heap-backed Vec + iterator + fold, then format! into a heap String.
    let squares: Vec<u64> = (1..=8u64).map(|n| n * n).collect();
    let sum: u64 = squares.iter().copied().sum();
    let s: String = format!(
        "Rust alloc on 9front: sum of squares 1..8 = {} (len {})\n",
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
