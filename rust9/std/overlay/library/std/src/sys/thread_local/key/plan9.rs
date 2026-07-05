//! Plan 9 TLS keys over cc9's pthread key API (pthread_key_t == int).
use crate::ffi;
use crate::mem;

unsafe extern "C" {
    fn pthread_key_create(
        key: *mut ffi::c_int,
        destructor: unsafe extern "C" fn(*mut ffi::c_void),
    ) -> ffi::c_int;
    fn pthread_getspecific(key: ffi::c_int) -> *mut ffi::c_void;
    fn pthread_setspecific(key: ffi::c_int, value: *const ffi::c_void) -> ffi::c_int;
    fn pthread_key_delete(key: ffi::c_int) -> ffi::c_int;
}

pub type Key = ffi::c_int;

#[inline]
pub fn create(dtor: Option<unsafe extern "C" fn(*mut u8)>) -> Key {
    let mut key = 0;
    if unsafe { pthread_key_create(&mut key, mem::transmute(dtor)) } != 0 {
        rtabort!("out of TLS keys");
    }
    key
}

#[inline]
pub unsafe fn set(key: Key, value: *mut u8) {
    let r = unsafe { pthread_setspecific(key, value as *mut _) };
    debug_assert_eq!(r, 0);
}

#[inline]
#[cfg(any(not(target_thread_local), test))]
pub unsafe fn get(key: Key) -> *mut u8 {
    unsafe { pthread_getspecific(key) as *mut u8 }
}

#[inline]
pub unsafe fn destroy(key: Key) {
    let r = unsafe { pthread_key_delete(key) };
    debug_assert_eq!(r, 0);
}
