//! Plan 9 errno (cc9 exposes glibc-numbered errno via __n9_errno()).
use crate::io::ErrorKind;

unsafe extern "C" {
    fn __n9_errno() -> *mut i32;
    // Last kernel errstr + the errno cc9 mapped it to (cc9/runtime/fs.c).
    fn __n9_errstr_last(errno_out: *mut i32) -> *const u8;
}

pub fn errno() -> i32 {
    unsafe { *__n9_errno() }
}

pub fn is_interrupted(code: i32) -> bool {
    code == 4 // EINTR
}

pub fn decode_error_kind(code: i32) -> ErrorKind {
    // cc9 exposes glibc-numbered errno (see cc9/runtime/include/errno.h); map the
    // subset cc9 actually produces (fs.c + the errstr table) to std ErrorKinds.
    use ErrorKind::*;
    match code {
        1 => PermissionDenied,   // EPERM
        2 => NotFound,           // ENOENT
        4 => Interrupted,        // EINTR
        5 => Uncategorized,      // EIO
        9 => Uncategorized,      // EBADF (no clean ErrorKind)
        11 => WouldBlock,        // EAGAIN
        12 => OutOfMemory,       // ENOMEM
        13 => PermissionDenied,  // EACCES
        16 => ResourceBusy,      // EBUSY
        17 => AlreadyExists,     // EEXIST
        18 => CrossesDevices,    // EXDEV
        20 => NotADirectory,     // ENOTDIR
        21 => IsADirectory,      // EISDIR
        22 => InvalidInput,      // EINVAL
        23 | 24 => TooManyOpenFiles, // ENFILE / EMFILE
        28 => StorageFull,       // ENOSPC
        30 => ReadOnlyFilesystem, // EROFS
        31 => TooManyLinks,      // EMLINK
        32 => BrokenPipe,        // EPIPE
        36 => InvalidFilename,   // ENAMETOOLONG
        38 => Unsupported,       // ENOSYS
        39 => DirectoryNotEmpty, // ENOTEMPTY
        40 => FilesystemLoop,    // ELOOP
        95 => Unsupported,       // ENOTSUP / EOPNOTSUPP
        110 => TimedOut,         // ETIMEDOUT
        _ => Uncategorized,
    }
}

pub fn error_string(errno: i32) -> String {
    // Plan 9 errors are strings; cc9 stashes the last kernel errstr next to the
    // errno it mapped it to. Show the real string when it belongs to this errno
    // (a mismatch means the errno came from a non-errstr path — fall back).
    let mut stash_errno: i32 = 0;
    let s = unsafe { __n9_errstr_last(&mut stash_errno) };
    if !s.is_null() && stash_errno == errno {
        let msg = unsafe { crate::ffi::CStr::from_ptr(s as *const crate::ffi::c_char) };
        // io::Error's Display appends " (os error N)" itself.
        if let Ok(msg) = msg.to_str() {
            if !msg.is_empty() {
                return msg.to_string();
            }
        }
    }
    format!("os error {errno}")
}
