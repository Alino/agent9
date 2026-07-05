use crate::cell::UnsafeCell;
use crate::ffi::{c_int, c_ulong, c_void};
use crate::io::Error;
use crate::pin::Pin;

// Mirror of cc9's `pthread_mutex_t`
// (typedef struct { int sem; unsigned long owner; int count; int kind; }).
#[repr(C)]
pub(super) struct pthread_mutex_t {
    sem: c_int,
    owner: c_ulong,
    count: c_int,
    kind: c_int,
}

const PTHREAD_MUTEX_INITIALIZER: pthread_mutex_t =
    pthread_mutex_t { sem: 1, owner: 0, count: 0, kind: 0 };

unsafe extern "C" {
    fn pthread_mutex_init(m: *mut pthread_mutex_t, attr: *const c_void) -> c_int;
    fn pthread_mutex_lock(m: *mut pthread_mutex_t) -> c_int;
    fn pthread_mutex_trylock(m: *mut pthread_mutex_t) -> c_int;
    fn pthread_mutex_unlock(m: *mut pthread_mutex_t) -> c_int;
    fn pthread_mutex_destroy(m: *mut pthread_mutex_t) -> c_int;
}

pub struct Mutex {
    inner: UnsafeCell<pthread_mutex_t>,
}

impl Mutex {
    pub fn new() -> Mutex {
        Mutex { inner: UnsafeCell::new(PTHREAD_MUTEX_INITIALIZER) }
    }

    pub(super) fn raw(&self) -> *mut pthread_mutex_t {
        self.inner.get()
    }

    /// # Safety
    /// May only be called once per instance of `Self`.
    pub unsafe fn init(self: Pin<&mut Self>) {
        // cc9's default mutex kind is PTHREAD_MUTEX_NORMAL (0) — deadlocks on a
        // same-thread re-lock, which is exactly what std wants (avoids aliasing
        // &mut from a reentrant lock). No attr dance needed.
        pthread_mutex_init(self.raw(), core::ptr::null());
    }

    /// # Safety
    /// * `init` must have been called.
    /// * A locked mutex must not be destroyed.
    pub unsafe fn lock(self: Pin<&Self>) {
        let r = pthread_mutex_lock(self.raw());
        if r != 0 {
            panic!("failed to lock mutex: {}", Error::from_raw_os_error(r));
        }
    }

    /// # Safety
    /// See `lock`.
    pub unsafe fn try_lock(self: Pin<&Self>) -> bool {
        pthread_mutex_trylock(self.raw()) == 0
    }

    /// # Safety
    /// The mutex must be locked by the current thread.
    pub unsafe fn unlock(self: Pin<&Self>) {
        let r = pthread_mutex_unlock(self.raw());
        debug_assert_eq!(r, 0);
    }
}

impl !Unpin for Mutex {}

unsafe impl Send for Mutex {}
unsafe impl Sync for Mutex {}

impl Drop for Mutex {
    fn drop(&mut self) {
        let r = unsafe { pthread_mutex_destroy(self.raw()) };
        debug_assert_eq!(r, 0);
    }
}
