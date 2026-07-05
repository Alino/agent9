//! Plan 9 pipes over cc9's n9_pipe (a bidirectional pipe; we use one fd per end).
use crate::fmt;
use crate::io::{self, BorrowedCursor, IoSlice, IoSliceMut};

unsafe extern "C" {
    fn n9_pipe(fds: *mut i32) -> i64;
    #[link_name = "read"]
    fn c_read(fd: i32, buf: *mut u8, n: usize) -> isize;
    #[link_name = "write"]
    fn c_write(fd: i32, buf: *const u8, n: usize) -> isize;
    #[link_name = "close"]
    fn c_close(fd: i32) -> i32;
    #[link_name = "dup"]
    fn c_dup(fd: i32) -> i32;
}

pub struct Pipe {
    fd: i32,
}

impl Pipe {
    pub(crate) fn from_fd(fd: i32) -> Pipe {
        Pipe { fd }
    }
    pub(crate) fn fd(&self) -> i32 {
        self.fd
    }
    pub(crate) fn into_fd(self) -> i32 {
        let fd = self.fd;
        crate::mem::forget(self);
        fd
    }

    pub fn try_clone(&self) -> io::Result<Self> {
        let fd = unsafe { c_dup(self.fd) };
        if fd < 0 { Err(io::Error::last_os_error()) } else { Ok(Pipe { fd }) }
    }

    pub fn read(&self, buf: &mut [u8]) -> io::Result<usize> {
        let n = unsafe { c_read(self.fd, buf.as_mut_ptr(), buf.len()) };
        if n < 0 { Err(io::Error::last_os_error()) } else { Ok(n as usize) }
    }
    pub fn read_buf(&self, buf: BorrowedCursor<'_, u8>) -> io::Result<()> {
        crate::io::default_read_buf(|b| self.read(b), buf)
    }
    pub fn read_vectored(&self, bufs: &mut [IoSliceMut<'_>]) -> io::Result<usize> {
        for b in bufs {
            if !b.is_empty() {
                return self.read(b);
            }
        }
        Ok(0)
    }
    pub fn is_read_vectored(&self) -> bool {
        false
    }
    pub fn read_to_end(&self, buf: &mut Vec<u8>) -> io::Result<usize> {
        let mut total = 0;
        let mut tmp = [0u8; 8192];
        loop {
            let n = self.read(&mut tmp)?;
            if n == 0 {
                break;
            }
            buf.extend_from_slice(&tmp[..n]);
            total += n;
        }
        Ok(total)
    }

    pub fn write(&self, buf: &[u8]) -> io::Result<usize> {
        let n = unsafe { c_write(self.fd, buf.as_ptr(), buf.len()) };
        if n < 0 { Err(io::Error::last_os_error()) } else { Ok(n as usize) }
    }
    pub fn write_vectored(&self, bufs: &[IoSlice<'_>]) -> io::Result<usize> {
        for b in bufs {
            if !b.is_empty() {
                return self.write(b);
            }
        }
        Ok(0)
    }
    pub fn is_write_vectored(&self) -> bool {
        false
    }

    pub fn diverge(&self) -> ! {
        panic!("Pipe::diverge on a real plan9 pipe")
    }
}

impl Drop for Pipe {
    fn drop(&mut self) {
        unsafe { c_close(self.fd) };
    }
}

impl fmt::Debug for Pipe {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Pipe").field("fd", &self.fd).finish()
    }
}

pub fn pipe() -> io::Result<(Pipe, Pipe)> {
    let mut fds = [0i32; 2];
    if unsafe { n9_pipe(fds.as_mut_ptr()) } < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok((Pipe { fd: fds[0] }, Pipe { fd: fds[1] }))
}
