//! Plan 9 environment: cc9 crt0 builds `environ` from the /env filesystem and
//! exposes getenv/setenv; unset removes the /env file.
pub use super::common::Env;
use crate::ffi::{CStr, OsStr, OsString, c_char};
use crate::io;

unsafe extern "C" {
    static environ: *const *const u8;
    #[link_name = "getenv"]
    fn c_getenv(name: *const u8) -> *const u8;
    #[link_name = "setenv"]
    fn c_setenv(name: *const u8, value: *const u8, overwrite: i32) -> i32;
    #[link_name = "remove"]
    fn c_remove(path: *const u8) -> i32;
}

unsafe fn os_from_bytes(bytes: &[u8]) -> OsString {
    unsafe { OsStr::from_encoded_bytes_unchecked(bytes).to_os_string() }
}

pub fn env() -> Env {
    let mut result = Vec::new();
    unsafe {
        let mut ptr = environ;
        if !ptr.is_null() {
            while !(*ptr).is_null() {
                let bytes = CStr::from_ptr(*ptr as *const c_char).to_bytes();
                if let Some(kv) = parse(bytes) {
                    result.push(kv);
                }
                ptr = ptr.add(1);
            }
        }
    }
    return Env::new(result);

    fn parse(input: &[u8]) -> Option<(OsString, OsString)> {
        if input.is_empty() {
            return None;
        }
        // Split on the first '=' after position 0 (glibc rule).
        let pos = input[1..].iter().position(|&b| b == b'=').map(|p| p + 1)?;
        unsafe { Some((os_from_bytes(&input[..pos]), os_from_bytes(&input[pos + 1..]))) }
    }
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
