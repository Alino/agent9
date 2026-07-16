use crate::os::fd::AsRawFd;

/// cc9's `isatty(fd)` is `fd >= 0 && fd <= 2 && $TERM set` — i.e. true only for
/// stdin/stdout/stderr when running under a console/terminal (alacritty9, rio, a
/// real cons), false when the stream is a file or a pipe. That's exactly the
/// `is_terminal` contract, so route straight through it. (unix uses `libc::isatty`;
/// plan9 isn't `target_family="unix"`, so it needs its own arm.)
pub fn is_terminal(fd: &impl AsRawFd) -> bool {
    unsafe extern "C" {
        fn isatty(fd: i32) -> i32;
    }
    unsafe { isatty(fd.as_raw_fd()) != 0 }
}
