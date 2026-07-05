use super::Mutex;
use crate::cell::UnsafeCell;
use crate::ffi::{c_int, c_void};
use crate::pin::Pin;
use crate::time::Duration;

// Mirror of cc9's `pthread_cond_t`
// (typedef struct { int lk; int pad; void *head; void *tail; }).
#[repr(C)]
struct pthread_cond_t {
    lk: c_int,
    pad: c_int,
    head: *mut c_void,
    tail: *mut c_void,
}

const PTHREAD_COND_INITIALIZER: pthread_cond_t =
    pthread_cond_t { lk: 1, pad: 0, head: core::ptr::null_mut(), tail: core::ptr::null_mut() };

#[repr(C)]
struct CTimespec {
    tv_sec: i64,
    tv_nsec: i64,
}

unsafe extern "C" {
    fn pthread_cond_init(c: *mut pthread_cond_t, attr: *const c_void) -> c_int;
    fn pthread_cond_signal(c: *mut pthread_cond_t) -> c_int;
    fn pthread_cond_broadcast(c: *mut pthread_cond_t) -> c_int;
    fn pthread_cond_wait(c: *mut pthread_cond_t, m: *mut c_void) -> c_int;
    fn pthread_cond_timedwait(c: *mut pthread_cond_t, m: *mut c_void, abstime: *const CTimespec)
    -> c_int;
    fn pthread_cond_destroy(c: *mut pthread_cond_t) -> c_int;
    fn clock_gettime(clk: c_int, ts: *mut CTimespec) -> c_int;
}

pub struct Condvar {
    inner: UnsafeCell<pthread_cond_t>,
}

impl Condvar {
    pub fn new() -> Condvar {
        Condvar { inner: UnsafeCell::new(PTHREAD_COND_INITIALIZER) }
    }

    fn raw(&self) -> *mut pthread_cond_t {
        self.inner.get()
    }

    // cc9's pthread_cond_timedwait timeout reliability is not audited, so let the
    // wrapper re-check elapsed time against `Instant`.
    pub const PRECISE_TIMEOUT: bool = false;

    /// # Safety
    /// May only be called once per instance of `Self`.
    pub unsafe fn init(self: Pin<&mut Self>) {
        pthread_cond_init(self.raw(), core::ptr::null());
    }

    /// # Safety
    /// `init` must have been called.
    pub unsafe fn notify_one(self: Pin<&Self>) {
        let r = pthread_cond_signal(self.raw());
        debug_assert_eq!(r, 0);
    }

    /// # Safety
    /// `init` must have been called.
    pub unsafe fn notify_all(self: Pin<&Self>) {
        let r = pthread_cond_broadcast(self.raw());
        debug_assert_eq!(r, 0);
    }

    /// # Safety
    /// * `init` must have been called.
    /// * `mutex` must be locked by the current thread, used with this condvar only.
    pub unsafe fn wait(self: Pin<&Self>, mutex: Pin<&Mutex>) {
        let r = pthread_cond_wait(self.raw(), mutex.raw() as *mut c_void);
        debug_assert_eq!(r, 0);
    }

    /// # Safety
    /// See `wait`.
    pub unsafe fn wait_timeout(&self, mutex: Pin<&Mutex>, dur: Duration) -> bool {
        // Absolute deadline (POSIX pthread_cond_timedwait takes an absolute time).
        let mut now = CTimespec { tv_sec: 0, tv_nsec: 0 };
        clock_gettime(0 /* CLOCK_REALTIME */, &mut now);
        let total_ns = now.tv_nsec as u128 + u128::from(dur.subsec_nanos());
        let mut sec = now.tv_sec as i128 + dur.as_secs() as i128 + (total_ns / 1_000_000_000) as i128;
        let nsec = (total_ns % 1_000_000_000) as i64;
        if sec > i64::MAX as i128 {
            sec = i64::MAX as i128;
        }
        let abstime = CTimespec { tv_sec: sec as i64, tv_nsec: nsec };
        let r = pthread_cond_timedwait(self.raw(), mutex.raw() as *mut c_void, &abstime);
        r == 0
    }
}

impl !Unpin for Condvar {}

unsafe impl Send for Condvar {}
unsafe impl Sync for Condvar {}

impl Drop for Condvar {
    fn drop(&mut self) {
        let r = unsafe { pthread_cond_destroy(self.raw()) };
        debug_assert_eq!(r, 0);
    }
}
