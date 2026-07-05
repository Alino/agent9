//! Plan 9 (9front, via the cc9 runtime) System allocator.
//! Backs `System`/the default global allocator with cc9's thread-safe
//! malloc/calloc/free/realloc/posix_memalign.
use super::{MIN_ALIGN, realloc_fallback};
use crate::alloc::{GlobalAlloc, Layout, System};
use crate::mem;
use crate::ptr;

unsafe extern "C" {
    fn malloc(size: usize) -> *mut u8;
    fn calloc(nmemb: usize, size: usize) -> *mut u8;
    fn realloc(ptr: *mut u8, size: usize) -> *mut u8;
    fn free(ptr: *mut u8);
    fn posix_memalign(memptr: *mut *mut u8, align: usize, size: usize) -> i32;
}

#[stable(feature = "alloc_system_type", since = "1.28.0")]
unsafe impl GlobalAlloc for System {
    #[inline]
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if layout.align() <= MIN_ALIGN && layout.align() <= layout.size() {
            unsafe { malloc(layout.size()) }
        } else {
            unsafe { aligned_malloc(&layout) }
        }
    }

    #[inline]
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        if layout.align() <= MIN_ALIGN && layout.align() <= layout.size() {
            unsafe { calloc(layout.size(), 1) }
        } else {
            let ptr = unsafe { self.alloc(layout) };
            if !ptr.is_null() {
                unsafe { ptr::write_bytes(ptr, 0, layout.size()) };
            }
            ptr
        }
    }

    #[inline]
    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        unsafe { free(ptr) }
    }

    #[inline]
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if layout.align() <= MIN_ALIGN && layout.align() <= new_size {
            unsafe { realloc(ptr, new_size) }
        } else {
            unsafe { realloc_fallback(self, ptr, layout, new_size) }
        }
    }
}

#[inline]
unsafe fn aligned_malloc(layout: &Layout) -> *mut u8 {
    let mut out = ptr::null_mut();
    // posix_memalign requires the alignment be a multiple of sizeof(void*).
    let align = layout.align().max(mem::size_of::<usize>());
    let ret = unsafe { posix_memalign(&mut out, align, layout.size()) };
    if ret != 0 { ptr::null_mut() } else { out as *mut u8 }
}
