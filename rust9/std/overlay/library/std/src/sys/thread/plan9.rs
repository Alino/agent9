//! Plan 9 threads over cc9's pthreads (rfork(RFMEM) + semaphores).
use crate::ffi::{CStr, c_void};
use crate::io;
use crate::mem::ManuallyDrop;
use crate::num::NonZero;
use crate::ptr;
use crate::thread::ThreadInit;
use crate::time::Duration;

type Pthread = u64; // cc9 pthread_t == unsigned long

unsafe extern "C" {
    fn pthread_create(
        thread: *mut Pthread,
        attr: *const c_void,
        start: extern "C" fn(*mut c_void) -> *mut c_void,
        arg: *mut c_void,
    ) -> i32;
    fn pthread_join(thread: Pthread, retval: *mut *mut c_void) -> i32;
    fn pthread_detach(thread: Pthread) -> i32;
    fn pthread_self() -> Pthread;
    fn sched_yield() -> i32;
    fn nanosleep(req: *const CTimespec, rem: *mut CTimespec) -> i32;
    // cc9's pthread stacks default to 256K; pass the requested size per-create.
    fn pthread_attr_init(attr: *mut CPthreadAttr) -> i32;
    fn pthread_attr_setstacksize(attr: *mut CPthreadAttr, size: usize) -> i32;
    fn pthread_attr_destroy(attr: *mut CPthreadAttr) -> i32;
}

/// cc9's `pthread_attr_t` (runtime/include/pthread.h). Layout must match.
#[repr(C)]
struct CPthreadAttr {
    detachstate: i32,
    stacksize: usize,
}

#[repr(C)]
struct CTimespec {
    tv_sec: i64,
    tv_nsec: i64,
}

pub const DEFAULT_MIN_STACK_SIZE: usize = 2 * 1024 * 1024;

pub struct Thread {
    id: Pthread,
}

unsafe impl Send for Thread {}
unsafe impl Sync for Thread {}

impl Thread {
    // unsafe: see thread::Builder::spawn_unchecked for safety requirements
    pub unsafe fn new(stack: usize, init: Box<ThreadInit>) -> io::Result<Thread> {
        let data = Box::into_raw(init);
        let mut native: Pthread = 0;
        // Pass the requested stack size per-create via attr. cc9's stacks default to
        // 256K and deeply-recursive callers (rustc's 8M compile thread, stylo/rayon
        // layout workers) overflow that. This used to go through the global
        // cc9_set_thread_stack(), which RACED: concurrent spawns clobbered each
        // other's size, so a worker could get 256K instead of its 8M, smash its
        // return address recursing, and fault with pc pointing into the stack.
        let mut attr = CPthreadAttr { detachstate: 0, stacksize: 0 };
        unsafe { pthread_attr_init(&mut attr) };
        unsafe { pthread_attr_setstacksize(&mut attr, stack) };
        let ret = unsafe {
            pthread_create(
                &mut native,
                &attr as *const CPthreadAttr as *const c_void,
                thread_start,
                data as *mut c_void,
            )
        };
        unsafe { pthread_attr_destroy(&mut attr) };
        return if ret == 0 {
            Ok(Thread { id: native })
        } else {
            // The thread failed to start, so `data` was not consumed: reclaim it.
            drop(unsafe { Box::from_raw(data) });
            Err(io::Error::from_raw_os_error(ret))
        };

        extern "C" fn thread_start(data: *mut c_void) -> *mut c_void {
            unsafe {
                let init = Box::from_raw(data as *mut ThreadInit);
                let rust_start = init.init();
                rust_start();
            }
            ptr::null_mut()
        }
    }

    pub fn join(self) {
        let id = ManuallyDrop::new(self).id;
        let ret = unsafe { pthread_join(id, ptr::null_mut()) };
        assert!(ret == 0, "failed to join thread: {}", io::Error::from_raw_os_error(ret));
    }
}

impl Drop for Thread {
    fn drop(&mut self) {
        let ret = unsafe { pthread_detach(self.id) };
        debug_assert_eq!(ret, 0);
    }
}

pub fn available_parallelism() -> io::Result<NonZero<usize>> {
    // Plan 9 exposes one line per CPU in /dev/sysstat (what ape's
    // sysconf(_SC_NPROCESSORS_ONLN) reads). Count them; fall back to 1.
    let n = match crate::fs::read("/dev/sysstat") {
        Ok(data) => data.split(|&b| b == b'\n').filter(|l| !l.is_empty()).count(),
        Err(_) => 1,
    };
    Ok(NonZero::new(n.max(1)).unwrap())
}

pub fn current_os_id() -> Option<u64> {
    Some(unsafe { pthread_self() })
}

pub fn yield_now() {
    unsafe { sched_yield() };
}

pub fn set_name(_name: &CStr) {}

pub fn sleep(dur: Duration) {
    let ts = CTimespec { tv_sec: dur.as_secs() as i64, tv_nsec: dur.subsec_nanos() as i64 };
    unsafe { nanosleep(&ts, ptr::null_mut()) };
}
