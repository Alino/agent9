//! Plan 9 environment: cc9 crt0 builds `environ` from the /env filesystem and
//! exposes getenv/setenv; unset removes the /env file.
pub use super::common::Env;
use crate::ffi::{CStr, OsStr, OsString, c_char, c_void};
use crate::io;

#[repr(C)]
struct CDirent {
    d_ino: u64,
    d_type: u8,
    d_name: [c_char; 256],
}

unsafe extern "C" {
    #[link_name = "getenv"]
    fn c_getenv(name: *const u8) -> *const u8;
    #[link_name = "setenv"]
    fn c_setenv(name: *const u8, value: *const u8, overwrite: i32) -> i32;
    #[link_name = "remove"]
    fn c_remove(path: *const u8) -> i32;
    #[link_name = "opendir"]
    fn c_opendir(path: *const u8) -> *mut c_void;
    #[link_name = "readdir"]
    fn c_readdir(dir: *mut c_void) -> *mut CDirent;
    #[link_name = "closedir"]
    fn c_closedir(dir: *mut c_void) -> i32;
}

unsafe fn os_from_bytes(bytes: &[u8]) -> OsString {
    unsafe { OsStr::from_encoded_bytes_unchecked(bytes).to_os_string() }
}

/// Enumerate the environment by reading the `/env` filesystem LIVE (each file is a
/// var, name = filename, content = value), rather than the `environ` array cc9
/// snapshots once at startup. The snapshot goes stale the moment `set_var`/
/// `remove_var` run (they touch /env, not the array), which used to make
/// `env::vars()` disagree with `env::var()` — a set var wouldn't list, a removed
/// one still would. Reading /env keeps vars() and var() (both → /env) consistent.
pub fn env() -> Env {
    let mut result = Vec::new();
    let dir = unsafe { c_opendir(b"/env\0".as_ptr()) };
    if !dir.is_null() {
        loop {
            let ent = unsafe { c_readdir(dir) };
            if ent.is_null() {
                break;
            }
            let name = unsafe { CStr::from_ptr((*ent).d_name.as_ptr()) }.to_bytes();
            if name.is_empty() || name == b"." || name == b".." {
                continue;
            }
            // Read the value the same way `var()` does. Both go through /env, so a
            // set/removed var now agrees between vars() and var(). Residual, inherent
            // divergences (not worth papering over): cc9 getenv returns baked defaults
            // for PATH/SHELL when no /env file exists, so var("PATH") can be Some while
            // vars() omits it; an empty-valued var reads back as absent; and a name
            // >255 bytes is truncated by the dirent, so getenv on the truncated name
            // misses. All cosmetic; the set/remove coherence bug is what this fixes.
            let key = unsafe { os_from_bytes(name) };
            if let Some(val) = getenv(&key) {
                result.push((key, val));
            }
        }
        unsafe { c_closedir(dir) };
    }
    Env::new(result)
}

pub fn getenv(k: &OsStr) -> Option<OsString> {
    let mut key = k.as_encoded_bytes().to_vec();
    key.push(0);
    let v = unsafe { c_getenv(key.as_ptr()) };
    if v.is_null() {
        None
    } else {
        let bytes = unsafe { CStr::from_ptr(v as *const c_char) }.to_bytes().to_vec();
        Some(unsafe { os_from_bytes(&bytes) })
    }
}

pub unsafe fn setenv(k: &OsStr, v: &OsStr) -> io::Result<()> {
    let mut key = k.as_encoded_bytes().to_vec();
    key.push(0);
    let mut val = v.as_encoded_bytes().to_vec();
    val.push(0);
    let r = unsafe { c_setenv(key.as_ptr(), val.as_ptr(), 1) };
    if r == 0 { Ok(()) } else { Err(io::Error::last_os_error()) }
}

pub unsafe fn unsetenv(k: &OsStr) -> io::Result<()> {
    let mut path = b"/env/".to_vec();
    path.extend_from_slice(k.as_encoded_bytes());
    path.push(0);
    unsafe { c_remove(path.as_ptr()) };
    Ok(())
}
