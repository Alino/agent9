//! Networking on Plan 9 (9front) over the `/net` filesystem.
//!
//! STALE PREMISE — READ BEFORE EXTENDING THIS MODULE (2026-07-15):
//! this was written when cc9's `socket()` was a stub, so it speaks `/net`
//! directly. cc9 now has a REAL BSD socket layer (`runtime/net9.c`: socket,
//! bind, listen, accept, connect, get/setsockopt, sendmsg, recvmsg,
//! getaddrinfo; gate test `cc9/test/netgate.c`).
//!
//! That matters beyond tidiness. Here a TCP connection is TWO fds (`data` +
//! `ctl`, and closing `ctl` hangs up), so this module can never honestly
//! implement `std::os::fd`: `AsRawFd` could borrow `data`, but `IntoRawFd` and
//! `FromRawFd` are undefinable — one fd cannot own the connection and `ctl`
//! cannot be reconstructed from `data`. Without `std::os::fd` there is no
//! socket2 and no mio, and therefore no tokio, no hyper, and no Servo network
//! stack.
//!
//! Rewriting this on cc9's BSD sockets makes a socket ONE fd and unblocks all of
//! that. It is the critical path for servo9 — see the servo9-port memory.
//!
//! Plan 9 has no BSD socket layer of its own; networking is done by manipulating
//! files under `/net`. This module speaks that protocol directly with raw
//! open/read/write/close (provided by the cc9 runtime), mirroring plan9port's
//! dial(2)/announce(2):
//!
//!   TCP connect:  open /net/tcp/clone -> read conn number N ->
//!                 write "connect host!port" to the ctl fd ->
//!                 open /net/tcp/N/data for the byte stream.
//!   TCP announce: open /net/tcp/clone -> write "announce *!port" ->
//!                 open /net/tcp/N/listen (blocks) -> read the new conn M ->
//!                 open /net/tcp/M/data.
//!   Name lookup:  write "tcp!host!port" to /net/cs, read back
//!                 "/net/tcp/clone ip!port" lines.
//!
//!   UDP:          open /net/udp/clone -> "announce addr!port" -> "headers" ->
//!                 each read/write on /net/udp/N/data carries a 52-byte Udphdr
//!                 (raddr[16] laddr[16] ifcaddr[16] rport[2] lport[2]).
//!
//! Scope: TCP client + server and UDP are implemented. Per-socket options that
//! `/net` does not cleanly expose (timeouts, nonblocking, multicast) return
//! `Unsupported` rather than silently lying.
#![allow(dead_code)]

use crate::fmt;
use crate::io::{self, BorrowedCursor, IoSlice, IoSliceMut};
use crate::net::{Ipv4Addr, Ipv6Addr, Shutdown, SocketAddr, SocketAddrV4, ToSocketAddrs};
use crate::sync::Mutex;
use crate::sync::atomic::{AtomicU64, Ordering};
use crate::sys::unsupported;
use crate::time::Duration;

unsafe extern "C" {
    /// fd2path(2): ask an open fd for its own path. This is what lets a plan9
    /// TcpStream be rebuilt from nothing but a raw fd — verified on 9front:
    /// fd2path(data) == "/net/tcp/N/data".
    ///
    /// cc9 exposes the syscall under its own name (see cc9/runtime/fs.c:
    /// `extern long n9_fd2path(int, char *, int)`), not the POSIX-ish one.
    fn n9_fd2path(fd: i32, buf: *mut u8, nbuf: i32) -> isize;
    fn open(path: *const u8, flags: i32, ...) -> i32;
    fn close(fd: i32) -> i32;
    fn read(fd: i32, buf: *mut u8, n: usize) -> isize;
    fn write(fd: i32, buf: *const u8, n: usize) -> isize;
    fn pread(fd: i32, buf: *mut u8, n: usize, off: i64) -> isize;
    /// Plan 9 alarm(2): arm a note `ms` from now (0 cancels), returns the previous
    /// alarm's remaining ms. Per-PROC — and cc9 threads are separate procs, so this
    /// is effectively per-thread: one connection's timeout never disturbs another's.
    fn n9_alarm(ms: u64) -> i64;
}

/// Run a blocking /net op under a timeout (`timeout_ns`; 0 = no timeout). Plan 9
/// has no per-fd read/write deadline, so we arm alarm(2): it posts a note that
/// interrupts the blocking syscall, which cc9 surfaces as EINTR (fs.c). We tell OUR
/// fired alarm from an unrelated interrupt via alarm(0)'s return: if the alarm had
/// already fired its remaining is 0 (=> TimedOut); if something else interrupted us
/// the alarm is still pending with remaining > 0 (=> propagate EINTR, retryable).
/// Caveat: shares the per-thread alarm with cc9's CC9_PROF profiler — don't expect
/// net timeouts and profiling to coexist within one thread.
fn with_timeout<T>(timeout_ns: u64, op: impl FnOnce() -> io::Result<T>) -> io::Result<T> {
    if timeout_ns == 0 {
        return op();
    }
    // round up to >= 1ms (Plan 9 alarm is ms-grained); saturating_add so a clamped
    // u64::MAX-ns "effectively none" timeout can't overflow the round-up.
    let ms = (timeout_ns.saturating_add(999_999) / 1_000_000).max(1);
    unsafe { n9_alarm(ms) };
    let r = op();
    let remaining = unsafe { n9_alarm(0) }; // cancel + read remaining
    match r {
        Err(ref e) if e.kind() == io::ErrorKind::Interrupted && remaining == 0 => {
            Err(io::const_error!(io::ErrorKind::TimedOut, "operation timed out"))
        }
        other => other,
    }
}

/// Encode `Option<Duration>` timeout as ns (0 = None). std rejects a zero Duration
/// before it reaches us, so any `Some` is >= 1ns and never collides with the None
/// sentinel. Huge durations clamp to u64::MAX ns (~584 years — effectively none).
fn dur_to_ns(t: Option<Duration>) -> u64 {
    match t {
        Some(d) => d.as_nanos().min(u64::MAX as u128) as u64,
        None => 0,
    }
}
fn ns_to_dur(ns: u64) -> Option<Duration> {
    if ns == 0 { None } else { Some(Duration::from_nanos(ns)) }
}

const O_RDONLY: i32 = 0;
const O_RDWR: i32 = 2;

/// Owned Plan 9 file descriptor; closes on drop.
pub struct Fd(i32);

impl Fd {
    /// The raw descriptor. Only meaningful because a plan9 TcpStream is ONE fd
    /// (see the note on TcpStream) — this is what `std::os::fd` hands out.
    pub fn as_raw(&self) -> i32 {
        self.0
    }

    fn raw_read(&self, buf: &mut [u8]) -> io::Result<usize> {
        let r = unsafe { read(self.0, buf.as_mut_ptr(), buf.len()) };
        if r < 0 { Err(io::Error::last_os_error()) } else { Ok(r as usize) }
    }
    fn raw_write(&self, buf: &[u8]) -> io::Result<usize> {
        let r = unsafe { write(self.0, buf.as_ptr(), buf.len()) };
        if r < 0 { Err(io::Error::last_os_error()) } else { Ok(r as usize) }
    }
    fn write_all(&self, mut buf: &[u8]) -> io::Result<()> {
        while !buf.is_empty() {
            match self.raw_write(buf)? {
                0 => return Err(io::const_error!(io::ErrorKind::WriteZero, "write returned 0")),
                n => buf = &buf[n..],
            }
        }
        Ok(())
    }
}

/// These are what make `std::os::fd` real for plan9: a TcpStream is ONE fd, so
/// borrowing it as a `BorrowedFd` is meaningful and safe. os/fd/{raw,owned,net}.rs
/// build every public impl on top of these.
impl crate::os::fd::AsRawFd for Fd {
    #[inline]
    fn as_raw_fd(&self) -> crate::os::fd::RawFd {
        self.0
    }
}

impl crate::os::fd::AsFd for Fd {
    #[inline]
    fn as_fd(&self) -> crate::os::fd::BorrowedFd<'_> {
        // SAFETY: self.0 is open for the lifetime of self (Fd closes on drop),
        // and the borrow cannot outlive it.
        unsafe { crate::os::fd::BorrowedFd::borrow_raw(self.0) }
    }
}

impl Drop for Fd {
    fn drop(&mut self) {
        unsafe { close(self.0) };
    }
}

/// Open a `/net` path from a Rust string (NUL-terminate on the stack-ish heap).
fn open_path(path: &str, flags: i32) -> io::Result<Fd> {
    let mut c = Vec::with_capacity(path.len() + 1);
    c.extend_from_slice(path.as_bytes());
    c.push(0);
    let fd = unsafe { open(c.as_ptr(), flags) };
    if fd < 0 { Err(io::Error::last_os_error()) } else { Ok(Fd(fd)) }
}

/// The ctl read returns e.g. "         5" or "5"; take the first run of digits.
fn parse_conn_number(s: &[u8]) -> io::Result<u32> {
    let mut num: u32 = 0;
    let mut seen = false;
    for &b in s {
        if b.is_ascii_digit() {
            num = num.wrapping_mul(10).wrapping_add((b - b'0') as u32);
            seen = true;
        } else if seen {
            break;
        }
    }
    if seen {
        Ok(num)
    } else {
        Err(io::const_error!(io::ErrorKind::InvalidData, "bad /net ctl response"))
    }
}

/// Read the leading decimal connection number from a freshly-cloned ctl file.
fn read_conn_number(ctl: &Fd) -> io::Result<u32> {
    let mut buf = [0u8; 64];
    let n = ctl.raw_read(&mut buf)?;
    parse_conn_number(&buf[..n])
}

/// Same, for an fd we do NOT own (`Fd` would close it on drop).
///
/// pread at offset 0 rather than read: the ctl file answers with the connection
/// number at offset 0 however often it is asked, so this stays repeatable and
/// leaves the caller's file offset alone.
fn read_conn_number_borrowed(fd: i32) -> io::Result<u32> {
    let mut buf = [0u8; 64];
    let r = unsafe { pread(fd, buf.as_mut_ptr(), buf.len(), 0) };
    if r < 0 {
        return Err(io::Error::last_os_error());
    }
    parse_conn_number(&buf[..r as usize])
}

/// Format a SocketAddr the way `/net` wants it: "address!port".
fn plan9_addr(a: &SocketAddr) -> String {
    match a {
        SocketAddr::V4(v4) => format!("{}!{}", v4.ip(), v4.port()),
        SocketAddr::V6(v6) => format!("{}!{}", v6.ip(), v6.port()),
    }
}

/// Read and parse "ip!port" from a connection's remote/local file.
fn read_endpoint(proto: &str, conn: u32, which: &str) -> io::Result<SocketAddr> {
    // local/remote are read-only files; an ORDWR open is refused.
    let f = open_path(&format!("/net/{proto}/{conn}/{which}"), O_RDONLY)?;
    let mut buf = [0u8; 128];
    let n = f.raw_read(&mut buf)?;
    let line = core::str::from_utf8(&buf[..n]).unwrap_or("").trim();
    // "ip!port" possibly followed by more whitespace-separated fields.
    let first = line.split_whitespace().next().unwrap_or("");
    parse_bang_addr(first)
        .ok_or_else(|| io::const_error!(io::ErrorKind::InvalidData, "bad /net address"))
}

/// Set the IP TTL on a conversation's ctl file and remember it for the getter
/// (the kernel default is not readable back through /net, so `ttl()` before any
/// `set_ttl()` is Unsupported rather than a guess).
struct Ttl(crate::sync::atomic::AtomicU32); // 0 = never set

impl Ttl {
    const fn new() -> Ttl {
        Ttl(crate::sync::atomic::AtomicU32::new(0))
    }
    fn set(&self, ctl: &Fd, ttl: u32) -> io::Result<()> {
        if ttl == 0 || ttl > 255 {
            return Err(io::const_error!(io::ErrorKind::InvalidInput, "ttl out of range"));
        }
        ctl.write_all(format!("ttl {ttl}").as_bytes())?;
        self.0.store(ttl, crate::sync::atomic::Ordering::Relaxed);
        Ok(())
    }
    fn get(&self) -> io::Result<u32> {
        match self.0.load(crate::sync::atomic::Ordering::Relaxed) {
            0 => unsupported(),
            t => Ok(t),
        }
    }
}

/// Parse Plan 9 "ip!port" into a SocketAddr (IPv4 or IPv6).
fn parse_bang_addr(s: &str) -> Option<SocketAddr> {
    let (ip, port) = s.rsplit_once('!')?;
    let port: u16 = port.parse().ok()?;
    if let Ok(v4) = ip.parse::<Ipv4Addr>() {
        return Some(SocketAddr::from((v4, port)));
    }
    if let Ok(v6) = ip.parse::<Ipv6Addr>() {
        return Some(SocketAddr::from((v6, port)));
    }
    None
}

////////////////////////////////////////////////////////////////////////////////
// TcpStream
////////////////////////////////////////////////////////////////////////////////

pub struct TcpStream {
    /// The ONLY fd. This used to also hold the ctl fd, on the belief that
    /// "closing ctl hangs it up". That is FALSE — measured on 9front: the
    /// connection survives close(ctl) and data keeps working. dial(2) does
    /// exactly this (a nil cfdp closes ctl, returning only the data fd).
    ///
    /// It matters because one fd is what makes `std::os::fd` definable for
    /// plan9 — and socket2/mio/tokio, hence every Rust network stack, are
    /// written entirely in those types.
    ///
    /// NB: a *listener* is different — there ctl IS the announcement and must
    /// be held (see TcpListener below).
    data: Fd,
    conn: u32,
    ttl: Ttl,
    /// Read/write timeouts in ns (0 = none). Stored here because /net has no
    /// per-fd deadline knob; `read`/`write` arm alarm(2) around the blocking op
    /// (see `with_timeout`). AtomicU64 so `set_*_timeout(&self)` stays Sync.
    read_timeout: AtomicU64,
    write_timeout: AtomicU64,
}

/// Ask an fd which /net connection it belongs to. This is the whole reason a
/// plan9 socket can round-trip through a bare integer: fd2path(2) returns e.g.
/// "/net/tcp/9/data" or "/net/tcp/9/ctl", and the conn number is right there.
/// Verified live on 9front.
fn conn_of_fd(fd: i32) -> io::Result<u32> {
    let mut buf = [0u8; 256];
    let r = unsafe { n9_fd2path(fd, buf.as_mut_ptr(), buf.len() as i32) };
    if r < 0 {
        return Err(io::Error::last_os_error());
    }
    let end = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    let path = core::str::from_utf8(&buf[..end])
        .map_err(|_| io::const_error!(io::ErrorKind::InvalidData, "fd2path: not utf8"))?;
    // /net/<proto>/<conn>/<file>
    let mut it = path.rsplit('/');
    let _file = it.next();
    if let Some(n) = it.next().and_then(|n| n.parse::<u32>().ok()) {
        return Ok(n);
    }
    // A freshly cloned fd reports the path it was WALKED to — "/net/tcp/clone" —
    // which carries no connection number (measured on 9front, see
    // cc9/test/clonepath.c). This is the state mio hands us: it creates the
    // socket, wraps it, and dials afterwards. The number is what reading the ctl
    // file returns, which is how every Plan 9 dialer learns it.
    if path.ends_with("/clone") {
        return read_conn_number_borrowed(fd);
    }
    Err(io::const_error!(io::ErrorKind::InvalidInput, "fd is not under /net"))
}

impl TcpStream {
    /// Rebuild a TcpStream from a bare fd — what `std::os::fd::FromRawFd` needs.
    ///
    /// Possible only because a plan9 connection is ONE fd plus a conn directory
    /// that the fd can name: fd2path(2) gives "/net/tcp/N/data" (or ".../ctl"),
    /// and N is the whole identity. Takes ownership of `fd`.
    ///
    /// # Safety
    /// `fd` must be an open fd under /net that nothing else owns.
    /// Rebuild from a bare fd by asking fd2path(2) which /net connection it names.
    ///
    /// Deliberately accepts any file in the conn directory, not just `data`, and
    /// does not require a peer. mio's TcpStream::connect() creates the socket,
    /// wraps it with from_raw_fd, and dials only afterwards — so at this point the
    /// fd is still the clone/ctl fd of an unconnected conn dir. Demanding
    /// ".../data" here rejected that legitimate caller; the conn number is what we
    /// actually need, and it is in the path either way.
    pub unsafe fn from_raw_fd_plan9(fd: i32) -> io::Result<TcpStream> {
        let conn = conn_of_fd(fd)?;
        Ok(TcpStream {
            data: Fd(fd),
            conn,
            ttl: Ttl::new(),
            read_timeout: AtomicU64::new(0),
            write_timeout: AtomicU64::new(0),
        })
    }

    /// The single underlying fd. Named `socket()` because that is what
    /// `os/fd/net.rs`'s generic impls call.
    pub fn socket(&self) -> &Fd {
        &self.data
    }

    /// Reopen this connection's ctl. The conn directory outlives ctl (it is kept
    /// alive by `data`), so ctl is always recoverable: from the cached conn
    /// number here, or from a bare fd via fd2path(2).
    fn ctl(&self) -> io::Result<Fd> {
        open_path(&format!("/net/tcp/{}/ctl", self.conn), O_RDWR)
    }

    pub fn connect<A: ToSocketAddrs>(addr: A) -> io::Result<TcpStream> {
        let mut last = None;
        for a in addr.to_socket_addrs()? {
            match TcpStream::connect_addr(&a) {
                Ok(s) => return Ok(s),
                Err(e) => last = Some(e),
            }
        }
        Err(last.unwrap_or_else(|| io::const_error!(io::ErrorKind::InvalidInput, "no addresses")))
    }

    fn connect_addr(addr: &SocketAddr) -> io::Result<TcpStream> {
        let ctl = open_path("/net/tcp/clone", O_RDWR)?;
        let conn = read_conn_number(&ctl)?;
        ctl.write_all(format!("connect {}", plan9_addr(addr)).as_bytes())?;
        let data = open_path(&format!("/net/tcp/{conn}/data"), O_RDWR)?;
        // Drop ctl: the connection is up and `data` keeps the conn dir alive.
        drop(ctl);
        Ok(TcpStream {
            data,
            conn,
            ttl: Ttl::new(),
            read_timeout: AtomicU64::new(0),
            write_timeout: AtomicU64::new(0),
        })
    }

    pub fn connect_timeout(addr: &SocketAddr, dur: Duration) -> io::Result<TcpStream> {
        // Arm alarm(2) around the blocking `connect ...` ctl write in connect_addr;
        // if it fires, the write returns EINTR -> TimedOut and the half-open conn's
        // fds drop (closed), aborting the dial. Without this a dead host blocked for
        // minutes (the whole TCP handshake), ignoring the caller's deadline.
        with_timeout(dur_to_ns(Some(dur)), || TcpStream::connect_addr(addr))
    }

    pub fn set_read_timeout(&self, t: Option<Duration>) -> io::Result<()> {
        self.read_timeout.store(dur_to_ns(t), Ordering::Relaxed);
        Ok(())
    }
    pub fn set_write_timeout(&self, t: Option<Duration>) -> io::Result<()> {
        self.write_timeout.store(dur_to_ns(t), Ordering::Relaxed);
        Ok(())
    }
    pub fn read_timeout(&self) -> io::Result<Option<Duration>> {
        Ok(ns_to_dur(self.read_timeout.load(Ordering::Relaxed)))
    }
    pub fn write_timeout(&self) -> io::Result<Option<Duration>> {
        Ok(ns_to_dur(self.write_timeout.load(Ordering::Relaxed)))
    }

    pub fn peek(&self, _: &mut [u8]) -> io::Result<usize> {
        unsupported()
    }

    pub fn read(&self, buf: &mut [u8]) -> io::Result<usize> {
        with_timeout(self.read_timeout.load(Ordering::Relaxed), || self.data.raw_read(buf))
    }

    pub fn read_buf(&self, mut cursor: BorrowedCursor<'_, u8>) -> io::Result<()> {
        // Read into a stack buffer, then append into the cursor (safe; no uninit).
        let mut tmp = [0u8; 8192];
        let cap = cursor.capacity().min(tmp.len());
        let t = self.read_timeout.load(Ordering::Relaxed);
        let n = with_timeout(t, || self.data.raw_read(&mut tmp[..cap]))?;
        cursor.append(&tmp[..n]);
        Ok(())
    }

    pub fn read_vectored(&self, bufs: &mut [IoSliceMut<'_>]) -> io::Result<usize> {
        // No readv on /net; fill the first non-empty buffer.
        let t = self.read_timeout.load(Ordering::Relaxed);
        for b in bufs {
            if !b.is_empty() {
                return with_timeout(t, || self.data.raw_read(b));
            }
        }
        Ok(0)
    }
    pub fn is_read_vectored(&self) -> bool {
        false
    }

    pub fn write(&self, buf: &[u8]) -> io::Result<usize> {
        with_timeout(self.write_timeout.load(Ordering::Relaxed), || self.data.raw_write(buf))
    }

    pub fn write_vectored(&self, bufs: &[IoSlice<'_>]) -> io::Result<usize> {
        let t = self.write_timeout.load(Ordering::Relaxed);
        for b in bufs {
            if !b.is_empty() {
                return with_timeout(t, || self.data.raw_write(b));
            }
        }
        Ok(0)
    }
    pub fn is_write_vectored(&self) -> bool {
        false
    }

    /// Read from /net/tcp/N/remote rather than cache at construction: the file is
    /// the truth, and a TcpStream can exist before it has a peer at all — mio's
    /// connect() wraps the socket fd and dials afterwards, so a cached peer would
    /// have to be invented for a socket that is not connected yet.
    ///
    /// An un-dialled conn dir has a remote of `::!0` / `0.0.0.0!0`, which parses
    /// perfectly well into a SocketAddr — so it has to be rejected explicitly, or
    /// peer_addr() reports `[::]:0` for a socket with no peer instead of the
    /// NotConnected the caller is entitled to.
    pub fn peer_addr(&self) -> io::Result<SocketAddr> {
        let peer = read_endpoint("tcp", self.conn, "remote")?;
        if peer.ip().is_unspecified() && peer.port() == 0 {
            return Err(io::const_error!(io::ErrorKind::NotConnected, "socket is not connected"));
        }
        Ok(peer)
    }
    pub fn socket_addr(&self) -> io::Result<SocketAddr> {
        read_endpoint("tcp", self.conn, "local")
    }

    pub fn shutdown(&self, how: Shutdown) -> io::Result<()> {
        // /net TCP has no half-close primitive — the only teardown is "hangup",
        // which closes BOTH directions. So:
        //  - Read: no-op. Fully hanging up here would kill the WRITE side too and
        //    break the very common send-then-read-response / TLS-close_notify idiom
        //    (`stream.shutdown(Read)` used to tear down the whole connection).
        //    KNOWN TRADEOFF: the reverse idiom — thread B calling `shutdown(Read)` to
        //    unblock thread A's blocking `read()` — no longer works (nothing wakes the
        //    reader). /net can't satisfy both; protecting the far more common
        //    write-side pattern is the deliberate choice. Use a read timeout or close
        //    the stream to unblock a reader instead.
        //  - Write/Both: hangup. This is lossy for Write (it also stops reads), but
        //    it's the only way to signal EOF to the peer on /net.
        match how {
            Shutdown::Read => Ok(()),
            Shutdown::Write | Shutdown::Both => self.ctl()?.write_all(b"hangup").map(|_| ()),
        }
    }

    pub fn duplicate(&self) -> io::Result<TcpStream> {
        unsupported()
    }

    pub fn set_linger(&self, _: Option<Duration>) -> io::Result<()> {
        unsupported()
    }
    pub fn linger(&self) -> io::Result<Option<Duration>> {
        Ok(None)
    }
    pub fn set_nodelay(&self, _: bool) -> io::Result<()> {
        // /net/tcp is delay-off by default; accept as a no-op.
        Ok(())
    }
    pub fn nodelay(&self) -> io::Result<bool> {
        Ok(true)
    }
    pub fn set_keepalive(&self, _: bool) -> io::Result<()> {
        Ok(())
    }
    pub fn keepalive(&self) -> io::Result<bool> {
        Ok(false)
    }
    pub fn set_ttl(&self, ttl: u32) -> io::Result<()> {
        self.ttl.set(&self.ctl()?, ttl)
    }
    pub fn ttl(&self) -> io::Result<u32> {
        self.ttl.get()
    }
    pub fn take_error(&self) -> io::Result<Option<io::Error>> {
        Ok(None)
    }
    pub fn set_nonblocking(&self, _: bool) -> io::Result<()> {
        unsupported()
    }
}

impl fmt::Debug for TcpStream {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.peer_addr() {
            Ok(peer) => write!(f, "TcpStream(/net/tcp/{} -> {})", self.conn, peer),
            // No peer yet (or the conn dir went away): say so rather than invent one.
            Err(_) => write!(f, "TcpStream(/net/tcp/{})", self.conn),
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// TcpListener
////////////////////////////////////////////////////////////////////////////////

pub struct TcpListener {
    ctl: Fd,
    conn: u32,
    ttl: Ttl,
}

impl TcpListener {
    /// Rebuild from a bare fd. Unlike TcpStream, a listener's fd is its ctl —
    /// the announcement — so that is what fd2path resolves here.
    ///
    /// # Safety
    /// `fd` must be an open /net announce ctl fd that nothing else owns.
    pub unsafe fn from_raw_fd_plan9(fd: i32) -> io::Result<TcpListener> {
        let conn = conn_of_fd(fd)?;
        Ok(TcpListener { ctl: Fd(fd), conn, ttl: Ttl::new() })
    }

    /// The fd os/fd/net.rs's generic impls hand out. NB: unlike TcpStream, a
    /// listener legitimately keeps its ctl — that fd IS the announcement.
    pub fn socket(&self) -> &Fd {
        &self.ctl
    }

    pub fn bind<A: ToSocketAddrs>(addr: A) -> io::Result<TcpListener> {
        let a = addr
            .to_socket_addrs()?
            .next()
            .ok_or_else(|| io::const_error!(io::ErrorKind::InvalidInput, "no addresses"))?;
        let ctl = open_path("/net/tcp/clone", O_RDWR)?;
        let conn = read_conn_number(&ctl)?;
        // Announce the address that was actually asked for. "*" (all interfaces)
        // is only right for an unspecified bind address — hardcoding it, as this
        // did, quietly ignored `bind(("10.0.2.15", 0))` and listened everywhere;
        // socket_addr() then reports an unspecified local address, which Plan 9
        // will not route to ("no route"), so connecting to your own local_addr()
        // fails even though the listener is up.
        //
        // Port 0 still means "/net picks a free one" — hence socket_addr reads the
        // conn dir rather than echoing back what we asked for.
        let target = if a.ip().is_unspecified() {
            format!("*!{}", a.port())
        } else {
            plan9_addr(&a)
        };
        ctl.write_all(format!("announce {target}").as_bytes())?;
        Ok(TcpListener { ctl, conn, ttl: Ttl::new() })
    }

    /// Read /net/tcp/N/local rather than echo back the bind address.
    ///
    /// Caching the requested address broke the commonest pattern there is:
    /// `bind((host, 0))` announces an ephemeral port, and this then reported port
    /// **0** — so anything that binds to 0 and connects to its own local_addr()
    /// (every test server, and plenty of real ones) got "connection refused".
    /// The conn directory knows the port that was actually assigned; ask it.
    pub fn socket_addr(&self) -> io::Result<SocketAddr> {
        read_endpoint("tcp", self.conn, "local")
    }

    pub fn accept(&self) -> io::Result<(TcpStream, SocketAddr)> {
        // Opening .../listen blocks until an inbound connection; it yields a
        // fresh ctl file whose read gives the new connection number.
        let lctl = open_path(&format!("/net/tcp/{}/listen", self.conn), O_RDWR)?;
        let m = read_conn_number(&lctl)?;
        let data = open_path(&format!("/net/tcp/{m}/data"), O_RDWR)?;
        // The accepted connection's peer is in its own conn dir. If that cannot be
        // read the connection is unusable, so fail rather than substitute an
        // address that is not the peer (this used to fall back to the listener's
        // own address, i.e. claim the client was us).
        let peer = read_endpoint("tcp", m, "remote")?;
        // Same as connect: the accepted connection's ctl (here, the fd returned
        // by opening .../listen) is not load-bearing — `data` keeps conn dir M
        // alive, and TcpStream::ctl() reopens it on demand. Verified on 9front
        // for a dialed connection; the accepted case has the same structure.
        drop(lctl);
        Ok((
            TcpStream {
                data,
                conn: m,
                ttl: Ttl::new(),
                read_timeout: AtomicU64::new(0),
                write_timeout: AtomicU64::new(0),
            },
            peer,
        ))
    }

    pub fn duplicate(&self) -> io::Result<TcpListener> {
        unsupported()
    }
    pub fn set_ttl(&self, ttl: u32) -> io::Result<()> {
        self.ttl.set(&self.ctl, ttl)
    }
    pub fn ttl(&self) -> io::Result<u32> {
        self.ttl.get()
    }
    pub fn set_only_v6(&self, _: bool) -> io::Result<()> {
        unsupported()
    }
    pub fn only_v6(&self) -> io::Result<bool> {
        unsupported()
    }
    pub fn take_error(&self) -> io::Result<Option<io::Error>> {
        Ok(None)
    }
    pub fn set_nonblocking(&self, _: bool) -> io::Result<()> {
        unsupported()
    }
}

impl fmt::Debug for TcpListener {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.socket_addr() {
            Ok(local) => write!(f, "TcpListener(/net/tcp/{} @ {})", self.conn, local),
            Err(_) => write!(f, "TcpListener(/net/tcp/{})", self.conn),
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// UdpSocket — /net/udp in "headers" mode: every datagram on the data file is
// prefixed with a 52-byte Udphdr (9front ip.h):
//   raddr[16] laddr[16] ifcaddr[16] rport[2] lport[2]
// Addresses are 16-byte IPv6 form; IPv4 rides as ::ffff:a.b.c.d.
////////////////////////////////////////////////////////////////////////////////

const UDPHDR_LEN: usize = 52;
const UDP_MAX: usize = 65535;

/// Encode a SocketAddr's IP as the 16-byte Plan 9 (IPv6) wire form.
fn ip16(a: &SocketAddr) -> [u8; 16] {
    match a {
        SocketAddr::V4(v4) => v4.ip().to_ipv6_mapped().octets(),
        SocketAddr::V6(v6) => v6.ip().octets(),
    }
}

/// Decode a 16-byte Plan 9 address + port into a SocketAddr (unmapping v4).
fn addr16(bytes: &[u8], port: u16) -> SocketAddr {
    let mut o = [0u8; 16];
    o.copy_from_slice(&bytes[..16]);
    let v6 = Ipv6Addr::from(o);
    match v6.to_ipv4_mapped() {
        Some(v4) => SocketAddr::from((v4, port)),
        None => SocketAddr::from((v6, port)),
    }
}

pub struct UdpSocket {
    ctl: Fd,
    data: Fd,
    conn: u32,
    peer: Mutex<Option<SocketAddr>>, // set by connect(); filters recv(), targets send()
    ttl: Ttl,
    read_timeout: AtomicU64,  // ns; 0 = none (alarm(2) around the blocking recv)
    write_timeout: AtomicU64,
}

impl UdpSocket {
    /// Rebuild from a bare fd. `socket()` hands out the DATA fd, so that is what
    /// comes back here; ctl is reopened from the conn number fd2path gives us.
    ///
    /// # Safety
    /// `fd` must be an open /net/udp data fd that nothing else owns.
    pub unsafe fn from_raw_fd_plan9(fd: i32) -> io::Result<UdpSocket> {
        let conn = conn_of_fd(fd)?;
        let ctl = open_path(&format!("/net/udp/{conn}/ctl"), O_RDWR)?;
        Ok(UdpSocket {
            ctl,
            data: Fd(fd),
            conn,
            peer: Mutex::new(None),
            ttl: Ttl::new(),
            read_timeout: AtomicU64::new(0),
            write_timeout: AtomicU64::new(0),
        })
    }

    /// The fd os/fd/net.rs's generic impls hand out.
    pub fn socket(&self) -> &Fd {
        &self.data
    }

    pub fn bind<A: ToSocketAddrs>(addr: A) -> io::Result<UdpSocket> {
        let a = addr
            .to_socket_addrs()?
            .next()
            .ok_or_else(|| io::const_error!(io::ErrorKind::InvalidInput, "no addresses"))?;
        let ctl = open_path("/net/udp/clone", O_RDWR)?;
        let conn = read_conn_number(&ctl)?;
        let ann = if a.ip().is_unspecified() {
            format!("announce *!{}", a.port())
        } else {
            format!("announce {}", plan9_addr(&a))
        };
        ctl.write_all(ann.as_bytes())?;
        // Per-datagram Udphdr framing on the data file (needed for from-addrs).
        ctl.write_all(b"headers")?;
        let data = open_path(&format!("/net/udp/{conn}/data"), O_RDWR)?;
        Ok(UdpSocket {
            ctl,
            data,
            conn,
            peer: Mutex::new(None),
            ttl: Ttl::new(),
            read_timeout: AtomicU64::new(0),
            write_timeout: AtomicU64::new(0),
        })
    }
    pub fn peer_addr(&self) -> io::Result<SocketAddr> {
        self.peer
            .lock()
            .unwrap()
            .ok_or_else(|| io::const_error!(io::ErrorKind::NotConnected, "not connected"))
    }
    pub fn socket_addr(&self) -> io::Result<SocketAddr> {
        read_endpoint("udp", self.conn, "local")
    }
    pub fn recv_from(&self, buf: &mut [u8]) -> io::Result<(usize, SocketAddr)> {
        // One datagram per read; anything beyond the buffer is dropped (BSD-style).
        let mut tmp = vec![0u8; UDPHDR_LEN + buf.len().min(UDP_MAX)];
        let t = self.read_timeout.load(Ordering::Relaxed);
        let n = with_timeout(t, || self.data.raw_read(&mut tmp))?;
        if n < UDPHDR_LEN {
            return Err(io::const_error!(io::ErrorKind::InvalidData, "short udp header"));
        }
        let rport = u16::from_be_bytes([tmp[48], tmp[49]]);
        let from = addr16(&tmp[0..16], rport);
        let len = (n - UDPHDR_LEN).min(buf.len());
        buf[..len].copy_from_slice(&tmp[UDPHDR_LEN..UDPHDR_LEN + len]);
        Ok((len, from))
    }
    pub fn peek_from(&self, _: &mut [u8]) -> io::Result<(usize, SocketAddr)> {
        unsupported() // a /net read consumes the datagram; no MSG_PEEK equivalent
    }
    pub fn send_to(&self, buf: &[u8], dst: &SocketAddr) -> io::Result<usize> {
        // Header + payload must go in ONE write (one datagram). laddr/lport are
        // zero: the kernel fills them from the announced conversation.
        let mut pkt = Vec::with_capacity(UDPHDR_LEN + buf.len());
        pkt.extend_from_slice(&ip16(dst));
        pkt.extend_from_slice(&[0u8; 32]); // laddr + ifcaddr
        pkt.extend_from_slice(&dst.port().to_be_bytes());
        pkt.extend_from_slice(&[0u8; 2]); // lport
        pkt.extend_from_slice(buf);
        let t = self.write_timeout.load(Ordering::Relaxed);
        let n = with_timeout(t, || self.data.raw_write(&pkt))?;
        Ok(n.saturating_sub(UDPHDR_LEN))
    }
    pub fn duplicate(&self) -> io::Result<UdpSocket> {
        unsupported()
    }
    pub fn set_read_timeout(&self, t: Option<Duration>) -> io::Result<()> {
        self.read_timeout.store(dur_to_ns(t), Ordering::Relaxed);
        Ok(())
    }
    pub fn set_write_timeout(&self, t: Option<Duration>) -> io::Result<()> {
        self.write_timeout.store(dur_to_ns(t), Ordering::Relaxed);
        Ok(())
    }
    pub fn read_timeout(&self) -> io::Result<Option<Duration>> {
        Ok(ns_to_dur(self.read_timeout.load(Ordering::Relaxed)))
    }
    pub fn write_timeout(&self) -> io::Result<Option<Duration>> {
        Ok(ns_to_dur(self.write_timeout.load(Ordering::Relaxed)))
    }
    pub fn set_broadcast(&self, _: bool) -> io::Result<()> {
        unsupported()
    }
    pub fn broadcast(&self) -> io::Result<bool> {
        unsupported()
    }
    pub fn set_multicast_loop_v4(&self, _: bool) -> io::Result<()> {
        unsupported()
    }
    pub fn multicast_loop_v4(&self) -> io::Result<bool> {
        unsupported()
    }
    pub fn set_multicast_ttl_v4(&self, _: u32) -> io::Result<()> {
        unsupported()
    }
    pub fn multicast_ttl_v4(&self) -> io::Result<u32> {
        unsupported()
    }
    pub fn set_multicast_loop_v6(&self, _: bool) -> io::Result<()> {
        unsupported()
    }
    pub fn multicast_loop_v6(&self) -> io::Result<bool> {
        unsupported()
    }
    pub fn join_multicast_v4(&self, _: &Ipv4Addr, _: &Ipv4Addr) -> io::Result<()> {
        unsupported()
    }
    pub fn join_multicast_v6(&self, _: &Ipv6Addr, _: u32) -> io::Result<()> {
        unsupported()
    }
    pub fn leave_multicast_v4(&self, _: &Ipv4Addr, _: &Ipv4Addr) -> io::Result<()> {
        unsupported()
    }
    pub fn leave_multicast_v6(&self, _: &Ipv6Addr, _: u32) -> io::Result<()> {
        unsupported()
    }
    pub fn set_ttl(&self, ttl: u32) -> io::Result<()> {
        self.ttl.set(&self.ctl, ttl)
    }
    pub fn ttl(&self) -> io::Result<u32> {
        self.ttl.get()
    }
    pub fn take_error(&self) -> io::Result<Option<io::Error>> {
        Ok(None)
    }
    pub fn set_nonblocking(&self, _: bool) -> io::Result<()> {
        unsupported()
    }
    pub fn recv(&self, buf: &mut [u8]) -> io::Result<usize> {
        let peer = self.peer_addr()?;
        // Connected-socket semantics: drop datagrams from anyone else.
        loop {
            let (n, from) = self.recv_from(buf)?;
            if from == peer {
                return Ok(n);
            }
        }
    }
    pub fn peek(&self, _: &mut [u8]) -> io::Result<usize> {
        unsupported()
    }
    pub fn send(&self, buf: &[u8]) -> io::Result<usize> {
        let peer = self.peer_addr()?;
        self.send_to(buf, &peer)
    }
    pub fn connect<A: ToSocketAddrs>(&self, addr: A) -> io::Result<()> {
        let a = addr
            .to_socket_addrs()?
            .next()
            .ok_or_else(|| io::const_error!(io::ErrorKind::InvalidInput, "no addresses"))?;
        *self.peer.lock().unwrap() = Some(a);
        Ok(())
    }
}

impl fmt::Debug for UdpSocket {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "UdpSocket(/net/udp/{})", self.conn)
    }
}

////////////////////////////////////////////////////////////////////////////////
// Name resolution via /net/cs
////////////////////////////////////////////////////////////////////////////////

pub struct LookupHost {
    addrs: crate::vec::IntoIter<SocketAddr>,
}

impl Iterator for LookupHost {
    type Item = SocketAddr;
    fn next(&mut self) -> Option<SocketAddr> {
        self.addrs.next()
    }
}

pub fn lookup_host(host: &str, port: u16) -> io::Result<LookupHost> {
    // Numeric address: resolve locally, no /net/cs round-trip needed.
    if let Ok(v4) = host.parse::<Ipv4Addr>() {
        return Ok(LookupHost { addrs: vec![SocketAddr::from((v4, port))].into_iter() });
    }
    if let Ok(v6) = host.parse::<Ipv6Addr>() {
        return Ok(LookupHost { addrs: vec![SocketAddr::from((v6, port))].into_iter() });
    }

    // Ask the connection server: write "tcp!host!port", read back translations.
    //
    // The replies are read from OFFSET 0 — dial(2) does an explicit
    // `seek(fd, 0, 0)` between the query write and the reads. Reading from the
    // current offset (the end of what was just written) returns EOF immediately,
    // so every non-numeric hostname came back "host not found" while hget on the
    // same box resolved fine (webfs does the seek). pread with an explicit,
    // advancing offset is the same thing without needing a seek call.
    let cs = open_path("/net/cs", O_RDWR)?;
    cs.write_all(format!("tcp!{host}!{port}").as_bytes())?;
    let mut addrs = Vec::new();
    let mut buf = [0u8; 256];
    let mut off: i64 = 0;
    loop {
        let n = unsafe { pread(cs.0, buf.as_mut_ptr(), buf.len(), off) };
        if n < 0 {
            return Err(io::Error::last_os_error());
        }
        let n = n as usize;
        if n == 0 {
            break;
        }
        off += n as i64;
        let line = core::str::from_utf8(&buf[..n]).unwrap_or("");
        // Each reply: "/net/tcp/clone ip!port"; take the last field.
        if let Some(field) = line.split_whitespace().last() {
            if let Some(sa) = parse_bang_addr(field) {
                addrs.push(sa);
            }
        }
    }
    if addrs.is_empty() {
        Err(io::const_error!(io::ErrorKind::NotFound, "host not found"))
    } else {
        Ok(LookupHost { addrs: addrs.into_iter() })
    }
}

// Keep SocketAddrV4 referenced even if only used via From above (silences an
// unused-import lint on some cfgs).
#[allow(dead_code)]
fn _use_v4(_: SocketAddrV4) {}

// rebuild trigger
