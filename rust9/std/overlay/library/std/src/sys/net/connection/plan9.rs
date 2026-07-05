//! Networking on Plan 9 (9front) over the `/net` filesystem.
//!
//! Plan 9 has no BSD socket layer (cc9's `socket()` is a stub); networking is
//! done by manipulating files under `/net`. This module speaks that protocol
//! directly with raw open/read/write/close (provided by the cc9 runtime),
//! mirroring plan9port's dial(2)/announce(2):
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
use crate::sys::unsupported;
use crate::time::Duration;

unsafe extern "C" {
    fn open(path: *const u8, flags: i32, ...) -> i32;
    fn close(fd: i32) -> i32;
    fn read(fd: i32, buf: *mut u8, n: usize) -> isize;
    fn write(fd: i32, buf: *const u8, n: usize) -> isize;
}

const O_RDONLY: i32 = 0;
const O_RDWR: i32 = 2;

/// Owned Plan 9 file descriptor; closes on drop.
struct Fd(i32);

impl Fd {
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

/// Read the leading decimal connection number from a freshly-cloned ctl file.
/// The ctl read returns e.g. "         5" or "5"; take the first run of digits.
fn read_conn_number(ctl: &Fd) -> io::Result<u32> {
    let mut buf = [0u8; 64];
    let n = ctl.raw_read(&mut buf)?;
    let s = &buf[..n];
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
    data: Fd,
    ctl: Fd, // keep the connection open (closing ctl hangs it up)
    conn: u32,
    peer: SocketAddr,
    ttl: Ttl,
}

impl TcpStream {
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
        Ok(TcpStream { data, ctl, conn, peer: *addr, ttl: Ttl::new() })
    }

    pub fn connect_timeout(addr: &SocketAddr, _: Duration) -> io::Result<TcpStream> {
        // /net has no per-dial timeout knob we honor yet; do a plain connect.
        TcpStream::connect_addr(addr)
    }

    pub fn set_read_timeout(&self, _: Option<Duration>) -> io::Result<()> {
        unsupported()
    }
    pub fn set_write_timeout(&self, _: Option<Duration>) -> io::Result<()> {
        unsupported()
    }
    pub fn read_timeout(&self) -> io::Result<Option<Duration>> {
        Ok(None)
    }
    pub fn write_timeout(&self) -> io::Result<Option<Duration>> {
        Ok(None)
    }

    pub fn peek(&self, _: &mut [u8]) -> io::Result<usize> {
        unsupported()
    }

    pub fn read(&self, buf: &mut [u8]) -> io::Result<usize> {
        self.data.raw_read(buf)
    }

    pub fn read_buf(&self, mut cursor: BorrowedCursor<'_, u8>) -> io::Result<()> {
        // Read into a stack buffer, then append into the cursor (safe; no uninit).
        let mut tmp = [0u8; 8192];
        let cap = cursor.capacity().min(tmp.len());
        let n = self.data.raw_read(&mut tmp[..cap])?;
        cursor.append(&tmp[..n]);
        Ok(())
    }

    pub fn read_vectored(&self, bufs: &mut [IoSliceMut<'_>]) -> io::Result<usize> {
        // No readv on /net; fill the first non-empty buffer.
        for b in bufs {
            if !b.is_empty() {
                return self.data.raw_read(b);
            }
        }
        Ok(0)
    }
    pub fn is_read_vectored(&self) -> bool {
        false
    }

    pub fn write(&self, buf: &[u8]) -> io::Result<usize> {
        self.data.raw_write(buf)
    }

    pub fn write_vectored(&self, bufs: &[IoSlice<'_>]) -> io::Result<usize> {
        for b in bufs {
            if !b.is_empty() {
                return self.data.raw_write(b);
            }
        }
        Ok(0)
    }
    pub fn is_write_vectored(&self) -> bool {
        false
    }

    pub fn peer_addr(&self) -> io::Result<SocketAddr> {
        Ok(self.peer)
    }
    pub fn socket_addr(&self) -> io::Result<SocketAddr> {
        read_endpoint("tcp", self.conn, "local")
    }

    pub fn shutdown(&self, _: Shutdown) -> io::Result<()> {
        // Writing "hangup" to the ctl closes the connection both ways.
        self.ctl.write_all(b"hangup").map(|_| ())
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
}

impl fmt::Debug for TcpStream {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "TcpStream(/net/tcp/{} -> {})", self.conn, self.peer)
    }
}

////////////////////////////////////////////////////////////////////////////////
// TcpListener
////////////////////////////////////////////////////////////////////////////////

pub struct TcpListener {
    ctl: Fd,
    conn: u32,
    local: SocketAddr,
    ttl: Ttl,
}

impl TcpListener {
    pub fn bind<A: ToSocketAddrs>(addr: A) -> io::Result<TcpListener> {
        let a = addr
            .to_socket_addrs()?
            .next()
            .ok_or_else(|| io::const_error!(io::ErrorKind::InvalidInput, "no addresses"))?;
        let ctl = open_path("/net/tcp/clone", O_RDWR)?;
        let conn = read_conn_number(&ctl)?;
        // "announce *!port" listens on all local interfaces at the given port.
        ctl.write_all(format!("announce *!{}", a.port()).as_bytes())?;
        Ok(TcpListener { ctl, conn, local: a, ttl: Ttl::new() })
    }

    pub fn socket_addr(&self) -> io::Result<SocketAddr> {
        Ok(self.local)
    }

    pub fn accept(&self) -> io::Result<(TcpStream, SocketAddr)> {
        // Opening .../listen blocks until an inbound connection; it yields a
        // fresh ctl file whose read gives the new connection number.
        let lctl = open_path(&format!("/net/tcp/{}/listen", self.conn), O_RDWR)?;
        let m = read_conn_number(&lctl)?;
        let data = open_path(&format!("/net/tcp/{m}/data"), O_RDWR)?;
        let peer = read_endpoint("tcp", m, "remote").unwrap_or(self.local);
        Ok((TcpStream { data, ctl: lctl, conn: m, peer, ttl: Ttl::new() }, peer))
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
        write!(f, "TcpListener(/net/tcp/{} @ {})", self.conn, self.local)
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
}

impl UdpSocket {
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
        Ok(UdpSocket { ctl, data, conn, peer: Mutex::new(None), ttl: Ttl::new() })
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
        let n = self.data.raw_read(&mut tmp)?;
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
        let n = self.data.raw_write(&pkt)?;
        Ok(n.saturating_sub(UDPHDR_LEN))
    }
    pub fn duplicate(&self) -> io::Result<UdpSocket> {
        unsupported()
    }
    pub fn set_read_timeout(&self, _: Option<Duration>) -> io::Result<()> {
        unsupported()
    }
    pub fn set_write_timeout(&self, _: Option<Duration>) -> io::Result<()> {
        unsupported()
    }
    pub fn read_timeout(&self) -> io::Result<Option<Duration>> {
        unsupported()
    }
    pub fn write_timeout(&self) -> io::Result<Option<Duration>> {
        unsupported()
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
    let cs = open_path("/net/cs", O_RDWR)?;
    cs.write_all(format!("tcp!{host}!{port}").as_bytes())?;
    let mut addrs = Vec::new();
    let mut buf = [0u8; 256];
    loop {
        let n = cs.raw_read(&mut buf)?;
        if n == 0 {
            break;
        }
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
