//! Path/cwd helpers for Plan 9, backed by the cc9 C runtime (getcwd/chdir).
//! Plan 9 uses UTF-8 for all paths, so the byte<->str conversion is lossless.
use crate::ffi::{OsStr, OsString};
use crate::path::{self, PathBuf};
use crate::{fmt, io, iter, slice};

// Plan 9 has no conventional PATH separator, but `env::{split,join}_paths` must
// round-trip (rustc's linker setup calls `join_paths(..).unwrap()`). `:` never
// appears inside a UTF-8 multibyte sequence, so splitting/joining on it is safe.
const PATH_SEPARATOR: u8 = b':';

pub type SplitPaths<'a> = iter::Map<
    slice::Split<'a, u8, impl FnMut(&u8) -> bool + 'static>,
    impl FnMut(&[u8]) -> PathBuf + 'static,
>;

#[define_opaque(SplitPaths)]
pub fn split_paths(unparsed: &OsStr) -> SplitPaths<'_> {
    fn is_separator(&b: &u8) -> bool {
        b == PATH_SEPARATOR
    }
    fn into_pathbuf(part: &[u8]) -> PathBuf {
        // SAFETY: `part` is a `:`-delimited slice of valid OsStr bytes; `:` is an
        // ASCII byte that can't split a multibyte char, so the slice stays valid.
        PathBuf::from(unsafe { OsStr::from_encoded_bytes_unchecked(part) }.to_os_string())
    }
    unparsed.as_encoded_bytes().split(is_separator).map(into_pathbuf)
}

#[derive(Debug)]
pub struct JoinPathsError;

pub fn join_paths<I, T>(paths: I) -> Result<OsString, JoinPathsError>
where
    I: Iterator<Item = T>,
    T: AsRef<OsStr>,
{
    let mut joined = Vec::new();
    for (i, path) in paths.enumerate() {
        let path = path.as_ref().as_encoded_bytes();
        if i > 0 {
            joined.push(PATH_SEPARATOR);
        }
        if path.contains(&PATH_SEPARATOR) {
            return Err(JoinPathsError);
        }
        joined.extend_from_slice(path);
    }
    // SAFETY: `joined` is valid OsStr bytes joined by an ASCII separator.
    Ok(unsafe { OsStr::from_encoded_bytes_unchecked(&joined) }.to_os_string())
}

impl fmt::Display for JoinPathsError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "path segment contains separator `{}`", char::from(PATH_SEPARATOR))
    }
}

impl crate::error::Error for JoinPathsError {}

unsafe extern "C" {
    #[link_name = "getcwd"]
    fn c_getcwd(buf: *mut u8, size: usize) -> *mut u8;
    #[link_name = "chdir"]
    fn c_chdir(path: *const u8) -> i32;
    #[link_name = "getpid"]
    fn c_getpid() -> i32;
    #[link_name = "open"]
    fn c_open(path: *const u8, flags: i32) -> i32;
    #[link_name = "close"]
    fn c_close(fd: i32) -> i32;
    // fd2path over the exec image; Plan 9 resolves /proc/$pid/text to the real
    // binary path (verified: returns the delivered a.out path, not "/proc/N/text").
    fn cc9_fd2path_for_exe(fd: i32, buf: *mut u8, n: usize) -> isize;
}

/// current_exe: open the exec image `/proc/$pid/text` and fd2path it — the Plan 9
/// way to learn one's own binary path (used by tools that re-exec / locate sibling
/// resources). Was `unsupported()`; the cc9 primitive shipped for exactly this.
pub fn current_exe() -> io::Result<PathBuf> {
    let pid = unsafe { c_getpid() };
    let mut path = format!("/proc/{pid}/text").into_bytes();
    path.push(0);
    let fd = unsafe { c_open(path.as_ptr(), 0) };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    let mut buf = [0u8; 4096];
    let r = unsafe { cc9_fd2path_for_exe(fd, buf.as_mut_ptr(), buf.len()) };
    unsafe { c_close(fd) };
    if r < 0 {
        // The fd2path thunk doesn't set POSIX errno, and c_close above already
        // clobbered it — so last_os_error() here would report a stale/"Success"
        // errno. Return a self-describing error instead.
        return Err(io::Error::new(io::ErrorKind::Other, "fd2path on /proc/$pid/text failed"));
    }
    let len = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    match crate::str::from_utf8(&buf[..len]) {
        Ok(s) => Ok(PathBuf::from(s)),
        Err(_) => Err(io::Error::new(io::ErrorKind::InvalidData, "exe path is not valid UTF-8")),
    }
}

/// Plan 9's home is the lowercase `$home` env var (dirs-sys uses the same).
pub fn home_dir() -> Option<PathBuf> {
    crate::env::var_os("home").filter(|v| !v.is_empty()).map(PathBuf::from)
}

pub fn getcwd() -> io::Result<PathBuf> {
    let mut buf = vec![0u8; 4096];
    let r = unsafe { c_getcwd(buf.as_mut_ptr(), buf.len()) };
    if r.is_null() {
        return Err(io::Error::last_os_error());
    }
    let len = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    buf.truncate(len);
    match String::from_utf8(buf) {
        Ok(s) => Ok(PathBuf::from(s)),
        Err(_) => Err(io::Error::new(io::ErrorKind::InvalidData, "cwd is not valid UTF-8")),
    }
}

pub fn chdir(p: &path::Path) -> io::Result<()> {
    let s = p
        .as_os_str()
        .to_str()
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "path is not valid UTF-8"))?;
    let mut c = s.as_bytes().to_vec();
    c.push(0);
    if unsafe { c_chdir(c.as_ptr()) } == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

pub fn temp_dir() -> PathBuf {
    PathBuf::from("/tmp")
}
