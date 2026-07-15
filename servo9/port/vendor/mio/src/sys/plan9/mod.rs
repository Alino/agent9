//! mio backend for Plan 9 (9front) over cc9's `poll()`.
//!
//! # Why not the other backends
//!
//! Plan 9 has no epoll and no kqueue, so mio's `unix` backend (3982 lines) does
//! not apply. The nearest template is `wasip1` (poll-based) — but read its own
//! header: it admits it "only (barely) works using a single thread", has no
//! `Waker`, and cannot register while another thread polls, because it holds its
//! subscription lock across the poll call. Servo is heavily threaded and tokio
//! registers from arbitrary threads while a worker is parked in `poll`, so that
//! design is not usable here.
//!
//! # Design: poll() + self-pipe
//!
//! The classic portable reactor:
//!
//! * `select()` snapshots the registration table, **drops the lock**, and only
//!   then blocks in `poll(2)`. So `register`/`deregister` never wait on a poll.
//! * A self-pipe is registered in every poll. `register`, `reregister`,
//!   `deregister` and `Waker::wake` all write one byte to it, which breaks the
//!   in-flight poll so it re-reads the table. Without this, a registration made
//!   during a blocking poll would not be seen until the poll happened to return
//!   — which for an idle connection is "never".
//!
//! Plan 9 idiom note: the native answer to concurrency here is a proc per
//! connection blocking on a read, not a readiness reactor. cc9's `poll()` is
//! itself implemented that way (a reader thread per fd). This backend exists to
//! give tokio the epoll-shaped API it demands; it is emulation, and honestly so.
//!
//! Nothing here is a stub: every function does the real thing or returns a real
//! error.

use std::io;
use std::os::fd::RawFd;
#[cfg(all(debug_assertions, feature = "net"))]
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use crate::{Interest, Token};

// --- cc9 / Plan 9 C surface -------------------------------------------------

#[repr(C)]
#[derive(Copy, Clone)]
struct pollfd {
    fd: i32,
    events: i16,
    revents: i16,
}

// Must match cc9/runtime/include/poll.h.
const POLLIN: i16 = 0x001;
const POLLOUT: i16 = 0x004;
const POLLERR: i16 = 0x008;
const POLLHUP: i16 = 0x010;
const POLLNVAL: i16 = 0x020;

unsafe extern "C" {
    fn poll(fds: *mut pollfd, nfds: u64, timeout: i32) -> i32;
    fn pipe(fds: *mut i32) -> i32;
    fn read(fd: i32, buf: *mut u8, n: usize) -> isize;
    fn write(fd: i32, buf: *const u8, n: usize) -> isize;
    fn close(fd: i32) -> i32;
}

fn last_error() -> io::Error {
    io::Error::last_os_error()
}

// --- Event ------------------------------------------------------------------

/// A readiness event: which token, and what poll(2) reported.
#[derive(Debug, Clone, Copy)]
pub struct Event {
    pub token: Token,
    pub revents: i16,
}

pub type Events = Vec<Event>;

pub mod event {
    use super::{Event, POLLERR, POLLHUP, POLLIN, POLLNVAL, POLLOUT};
    use crate::Token;
    use std::fmt;

    pub fn token(event: &Event) -> Token {
        event.token
    }

    pub fn is_readable(event: &Event) -> bool {
        // HUP counts as readable: a peer that closed leaves a readable EOF, and
        // callers must be woken to observe it.
        event.revents & (POLLIN | POLLHUP) != 0
    }

    pub fn is_writable(event: &Event) -> bool {
        event.revents & POLLOUT != 0
    }

    pub fn is_error(event: &Event) -> bool {
        event.revents & (POLLERR | POLLNVAL) != 0
    }

    pub fn is_read_closed(event: &Event) -> bool {
        event.revents & POLLHUP != 0
    }

    pub fn is_write_closed(event: &Event) -> bool {
        // poll(2) reports one HUP for the whole connection; /net gives no
        // separate write-side shutdown signal, so this mirrors read_closed
        // rather than inventing a distinction.
        event.revents & POLLHUP != 0
    }

    pub fn is_priority(_: &Event) -> bool {
        // POLLPRI exists in the header but /net has no out-of-band channel.
        false
    }

    pub fn is_aio(_: &Event) -> bool {
        false
    }

    pub fn is_lio(_: &Event) -> bool {
        false
    }

    pub fn debug_details(f: &mut fmt::Formatter<'_>, event: &Event) -> fmt::Result {
        write!(f, "token: {:?}, revents: {:#06x}", event.token, event.revents)
    }
}

// --- Selector ---------------------------------------------------------------

#[derive(Debug, Clone, Copy)]
struct Registration {
    fd: RawFd,
    token: Token,
    interests: Interest,
}

#[derive(Debug)]
struct SelectorInner {
    /// Registered sources. Never locked across a `poll` call.
    regs: Mutex<Vec<Registration>>,
    /// Self-pipe. `wake_r` is polled every round; a byte on `wake_w` breaks the
    /// poll so a concurrent registration is picked up immediately.
    wake_r: RawFd,
    wake_w: RawFd,
}

impl Drop for SelectorInner {
    fn drop(&mut self) {
        unsafe {
            close(self.wake_r);
            close(self.wake_w);
        }
    }
}

impl SelectorInner {
    /// Break any in-flight `poll`. Best effort: if the pipe is already full the
    /// reader is guaranteed to wake anyway, so a failed write is not an error.
    fn interrupt(&self) {
        let b = [0u8; 1];
        unsafe {
            write(self.wake_w, b.as_ptr(), 1);
        }
    }

    fn drain_wake(&self) {
        let mut buf = [0u8; 64];
        // Drain what is buffered; the pipe is non-blocking-ish in effect because
        // we only read it after poll said it is readable.
        loop {
            let n = unsafe { read(self.wake_r, buf.as_mut_ptr(), buf.len()) };
            if n < buf.len() as isize {
                break;
            }
        }
    }
}

/// Only used to give each Selector a debug-only identity (see `Selector::id`),
/// so it does not exist in release.
#[cfg(all(debug_assertions, feature = "net"))]
static NEXT_ID: AtomicUsize = AtomicUsize::new(1);

#[derive(Debug)]
pub struct Selector {
    #[cfg(all(debug_assertions, feature = "net"))]
    id: usize,
    inner: Arc<SelectorInner>,
}

impl Selector {
    pub fn new() -> io::Result<Selector> {
        let mut fds = [0i32; 2];
        if unsafe { pipe(fds.as_mut_ptr()) } < 0 {
            return Err(last_error());
        }
        Ok(Selector {
            #[cfg(all(debug_assertions, feature = "net"))]
            id: NEXT_ID.fetch_add(1, Ordering::Relaxed),
            inner: Arc::new(SelectorInner {
                regs: Mutex::new(Vec::new()),
                wake_r: fds[0],
                wake_w: fds[1],
            }),
        })
    }

    #[cfg(all(debug_assertions, feature = "net"))]
    pub fn id(&self) -> usize {
        self.id
    }

    pub fn try_clone(&self) -> io::Result<Selector> {
        // Shares the registration table and the wake pipe, so a clone sees the
        // same sources and the same interrupts.
        Ok(Selector {
            #[cfg(all(debug_assertions, feature = "net"))]
            id: self.id,
            inner: self.inner.clone(),
        })
    }

    pub fn select(&self, events: &mut Events, timeout: Option<Duration>) -> io::Result<()> {
        events.clear();

        // Snapshot, then DROP the lock. Holding it across poll() is what makes
        // the wasip1 backend single-threaded-only.
        let snapshot: Vec<Registration> = {
            let regs = self.inner.regs.lock().unwrap();
            regs.clone()
        };

        let mut fds: Vec<pollfd> = Vec::with_capacity(snapshot.len() + 1);
        // Slot 0 is always the wake pipe.
        fds.push(pollfd { fd: self.inner.wake_r, events: POLLIN, revents: 0 });
        for r in &snapshot {
            let mut ev = 0i16;
            if r.interests.is_readable() {
                ev |= POLLIN;
            }
            if r.interests.is_writable() {
                ev |= POLLOUT;
            }
            fds.push(pollfd { fd: r.fd, events: ev, revents: 0 });
        }

        let timeout_ms = match timeout {
            // Saturate rather than wrap: a huge Duration must mean "a long
            // time", never a short one.
            Some(d) => d.as_millis().min(i32::MAX as u128) as i32,
            None => -1,
        };

        let n = unsafe { poll(fds.as_mut_ptr(), fds.len() as u64, timeout_ms) };
        if n < 0 {
            let err = last_error();
            // An interrupted poll is not a failure; report no events and let the
            // caller loop.
            if err.kind() == io::ErrorKind::Interrupted {
                return Ok(());
            }
            return Err(err);
        }
        if n == 0 {
            return Ok(()); // timed out
        }

        // Slot 0: we were interrupted (a registration changed, or Waker fired).
        if fds[0].revents & POLLIN != 0 {
            self.inner.drain_wake();
        }

        for (i, r) in snapshot.iter().enumerate() {
            let revents = fds[i + 1].revents;
            if revents != 0 {
                events.push(Event { token: r.token, revents });
            }
        }
        Ok(())
    }

    pub fn register(&self, fd: RawFd, token: Token, interests: Interest) -> io::Result<()> {
        {
            let mut regs = self.inner.regs.lock().unwrap();
            if regs.iter().any(|r| r.fd == fd) {
                return Err(io::Error::new(
                    io::ErrorKind::AlreadyExists,
                    "fd already registered",
                ));
            }
            regs.push(Registration { fd, token, interests });
        }
        // Wake any in-flight poll so it picks this up now, not whenever it
        // happens to return.
        self.inner.interrupt();
        Ok(())
    }

    pub fn reregister(&self, fd: RawFd, token: Token, interests: Interest) -> io::Result<()> {
        {
            let mut regs = self.inner.regs.lock().unwrap();
            match regs.iter_mut().find(|r| r.fd == fd) {
                Some(r) => {
                    r.token = token;
                    r.interests = interests;
                }
                None => return Err(io::ErrorKind::NotFound.into()),
            }
        }
        self.inner.interrupt();
        Ok(())
    }

    pub fn deregister(&self, fd: RawFd) -> io::Result<()> {
        {
            let mut regs = self.inner.regs.lock().unwrap();
            match regs.iter().position(|r| r.fd == fd) {
                Some(i) => {
                    regs.swap_remove(i);
                }
                None => return Err(io::ErrorKind::NotFound.into()),
            }
        }
        self.inner.interrupt();
        Ok(())
    }

    fn wake_handle(&self) -> Arc<SelectorInner> {
        self.inner.clone()
    }
}

// --- Waker ------------------------------------------------------------------

/// Wakes a `Poll` from another thread by writing to the selector's self-pipe.
#[derive(Debug)]
pub struct Waker {
    inner: Arc<SelectorInner>,
}

impl Waker {
    pub fn new(selector: &Selector, _token: Token) -> io::Result<Waker> {
        // The token is unused: an interrupt is not a source event. `select`
        // drains the pipe and reports nothing for it, which is what mio expects
        // of a Waker on backends where waking is not itself a readiness event.
        Ok(Waker { inner: selector.wake_handle() })
    }

    pub fn wake(&self) -> io::Result<()> {
        self.inner.interrupt();
        Ok(())
    }
}

// --- IoSourceState ----------------------------------------------------------

cfg_io_source! {
    use crate::Registry;

    // Like epoll/kqueue, poll(2) needs no user-space per-source state — the
    // registration table lives in the Selector. Mirrors
    // sys/unix/selector/stateless_io_source.rs.
    pub(crate) struct IoSourceState;

    impl IoSourceState {
        pub(crate) fn new() -> IoSourceState {
            IoSourceState
        }

        pub(crate) fn do_io<T, F, R>(&self, f: F, io: &T) -> io::Result<R>
        where
            F: FnOnce(&T) -> io::Result<R>,
        {
            f(io)
        }

        pub(crate) fn register(
            &mut self,
            registry: &Registry,
            token: Token,
            interests: Interest,
            fd: RawFd,
        ) -> io::Result<()> {
            registry.selector().register(fd, token, interests)
        }

        pub(crate) fn reregister(
            &mut self,
            registry: &Registry,
            token: Token,
            interests: Interest,
            fd: RawFd,
        ) -> io::Result<()> {
            registry.selector().reregister(fd, token, interests)
        }

        pub(crate) fn deregister(&mut self, registry: &Registry, fd: RawFd) -> io::Result<()> {
            registry.selector().deregister(fd)
        }
    }
}

// --- net --------------------------------------------------------------------

cfg_net! {
    use std::mem::{size_of, MaybeUninit};
    use std::net::{self, SocketAddr};
    use std::os::fd::{AsRawFd, FromRawFd};

    /// SocketAddr -> a cc9 `sockaddr` + length. Layouts come from
    /// cc9/runtime/include/netinet/in.h via the plan9 libc module.
    fn socket_addr(addr: &SocketAddr) -> (libc::sockaddr_storage, libc::socklen_t) {
        let mut storage: libc::sockaddr_storage = unsafe { std::mem::zeroed() };
        match addr {
            SocketAddr::V4(a) => {
                let sin = libc::sockaddr_in {
                    sin_family: libc::AF_INET as libc::sa_family_t,
                    sin_port: a.port().to_be(),
                    sin_addr: libc::in_addr { s_addr: u32::from_ne_bytes(a.ip().octets()) },
                    sin_zero: [0; 8],
                };
                unsafe {
                    std::ptr::copy_nonoverlapping(
                        &sin as *const _ as *const u8,
                        &mut storage as *mut _ as *mut u8,
                        size_of::<libc::sockaddr_in>(),
                    );
                }
                (storage, size_of::<libc::sockaddr_in>() as libc::socklen_t)
            }
            SocketAddr::V6(a) => {
                let sin6 = libc::sockaddr_in6 {
                    sin6_family: libc::AF_INET6 as libc::sa_family_t,
                    sin6_port: a.port().to_be(),
                    sin6_flowinfo: a.flowinfo(),
                    sin6_addr: libc::in6_addr { s6_addr: a.ip().octets() },
                    sin6_scope_id: a.scope_id(),
                };
                unsafe {
                    std::ptr::copy_nonoverlapping(
                        &sin6 as *const _ as *const u8,
                        &mut storage as *mut _ as *mut u8,
                        size_of::<libc::sockaddr_in6>(),
                    );
                }
                (storage, size_of::<libc::sockaddr_in6>() as libc::socklen_t)
            }
        }
    }

    fn new_socket(domain: i32, ty: i32) -> io::Result<i32> {
        let fd = unsafe { libc::socket(domain, ty, 0) };
        if fd < 0 {
            return Err(last_error());
        }
        // mio requires non-blocking sources; cc9 honours O_NONBLOCK via fcntl.
        let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
        if flags < 0 || unsafe { libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) } < 0 {
            let e = last_error();
            unsafe { libc::close(fd) };
            return Err(e);
        }
        Ok(fd)
    }

    pub(crate) mod tcp {
        use super::*;

        pub(crate) fn new_for_addr(address: SocketAddr) -> io::Result<i32> {
            let domain = match address {
                SocketAddr::V4(_) => libc::AF_INET,
                SocketAddr::V6(_) => libc::AF_INET6,
            };
            new_socket(domain, libc::SOCK_STREAM)
        }

        pub(crate) fn bind(socket: &net::TcpListener, addr: SocketAddr) -> io::Result<()> {
            let (raw, len) = socket_addr(&addr);
            let r = unsafe {
                libc::bind(socket.as_raw_fd(), &raw as *const _ as *const libc::sockaddr, len)
            };
            if r < 0 { Err(last_error()) } else { Ok(()) }
        }

        /// NB: cc9's connect() is SYNCHRONOUS even on an O_NONBLOCK socket — a
        /// /net connect is a write to the conn's ctl file, and there is no
        /// EINPROGRESS. So this blocks for the length of the TCP handshake
        /// instead of returning immediately and signalling writability later.
        /// Correct, but it stalls the calling reactor thread; see the servo9
        /// notes. Accept EINPROGRESS anyway in case net9 ever grows it.
        pub(crate) fn connect(socket: &net::TcpStream, addr: SocketAddr) -> io::Result<()> {
            let (raw, len) = socket_addr(&addr);
            let r = unsafe {
                libc::connect(socket.as_raw_fd(), &raw as *const _ as *const libc::sockaddr, len)
            };
            if r < 0 {
                let err = last_error();
                // EINPROGRESS (115 on linux) is the async-connect signal.
                if err.raw_os_error() != Some(115) {
                    return Err(err);
                }
            }
            Ok(())
        }

        pub(crate) fn listen(socket: &net::TcpListener, backlog: i32) -> io::Result<()> {
            let r = unsafe { libc::listen(socket.as_raw_fd(), backlog) };
            if r < 0 { Err(last_error()) } else { Ok(()) }
        }

        pub(crate) fn set_reuseaddr(socket: &net::TcpListener, reuseaddr: bool) -> io::Result<()> {
            let val: i32 = i32::from(reuseaddr);
            let r = unsafe {
                libc::setsockopt(
                    socket.as_raw_fd(),
                    libc::SOL_SOCKET,
                    libc::SO_REUSEADDR,
                    &val as *const i32 as *const std::ffi::c_void,
                    size_of::<i32>() as libc::socklen_t,
                )
            };
            if r < 0 { Err(last_error()) } else { Ok(()) }
        }

        pub(crate) fn accept(
            listener: &net::TcpListener,
        ) -> io::Result<(net::TcpStream, SocketAddr)> {
            // No accept4(2) on Plan 9; accept then set non-blocking.
            let mut addr: MaybeUninit<libc::sockaddr_storage> = MaybeUninit::uninit();
            let mut length = size_of::<libc::sockaddr_storage>() as libc::socklen_t;
            let fd = unsafe {
                libc::accept(
                    listener.as_raw_fd(),
                    addr.as_mut_ptr() as *mut libc::sockaddr,
                    &mut length,
                )
            };
            if fd < 0 {
                return Err(last_error());
            }
            let stream = unsafe { net::TcpStream::from_raw_fd(fd) };
            stream.set_nonblocking(true)?;
            let peer = stream.peer_addr()?;
            Ok((stream, peer))
        }
    }

    pub(crate) mod udp {
        use super::*;

        pub(crate) fn bind(addr: SocketAddr) -> io::Result<net::UdpSocket> {
            let socket = net::UdpSocket::bind(addr)?;
            socket.set_nonblocking(true)?;
            Ok(socket)
        }

        pub(crate) fn only_v6(_: &net::UdpSocket) -> io::Result<bool> {
            // /net exposes no IPV6_V6ONLY state to query.
            Err(io::ErrorKind::Unsupported.into())
        }
    }
}
