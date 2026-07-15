//! netgate.rs — does `std::net` still work on 9front after TcpStream dropped
//! its ctl fd?
//!
//! Modelled on `cc9/test/netgate.c`. This exists because the change it guards is
//! invisible to the compiler: TcpStream used to hold two fds (data + ctl) on the
//! belief that closing ctl hangs up the connection. That belief was false, and
//! dropping ctl is what makes `std::os::fd` — and therefore socket2/mio/tokio —
//! possible for plan9. If the belief had been *right*, everything would still
//! compile and connections would simply die. So: exercise real traffic.
//!
//! Build for plan9 and run on the box. Expects "netgate N/N PASS".
//!
//! NB: 127.0.0.1 does not route on cirno — pass a real address.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::thread;

fn main() {
    let host = std::env::args().nth(1).unwrap_or_else(|| "192.168.88.159".to_string());
    let mut pass = 0;
    let mut total = 0;

    // 1. listener + connect + echo, both directions. If dropping ctl broke the
    //    connection, this hangs or errors instead of echoing.
    total += 1;
    match echo_roundtrip(&host) {
        Ok(()) => {
            println!("1 echo roundtrip: PASS");
            pass += 1;
        }
        Err(e) => println!("1 echo roundtrip: FAIL {e}"),
    }

    // 2. peer_addr/local_addr must still resolve — they read /net/tcp/N/{local,
    //    remote}, i.e. the conn dir that `data` is supposed to be keeping alive.
    //    This is the direct test that the dir outlives ctl.
    total += 1;
    match addrs_readable(&host) {
        Ok(s) => {
            println!("2 conn dir alive after ctl drop: PASS ({s})");
            pass += 1;
        }
        Err(e) => println!("2 conn dir alive after ctl drop: FAIL {e}"),
    }

    // 3. a second connection while the first is open — proves we did not leave
    //    the conn dir in a state that blocks reuse.
    total += 1;
    match two_at_once(&host) {
        Ok(()) => {
            println!("3 two concurrent connections: PASS");
            pass += 1;
        }
        Err(e) => println!("3 two concurrent connections: FAIL {e}"),
    }

    // 4. a connected stream must survive a trip through a bare integer.
    //    std::os::fd is the currency socket2/mio/tokio are written in, so this is
    //    the shape every Rust network crate uses.
    total += 1;
    match raw_fd_roundtrip(&host) {
        Ok(s) => {
            println!("4 connected stream round-trips through a raw fd: PASS ({s})");
            pass += 1;
        }
        Err(e) => println!("4 connected stream round-trips through a raw fd: FAIL {e}"),
    }

    // 5. THE ONE THAT BROKE SERVO. mio's TcpStream::connect does
    //    socket() -> from_raw_fd() -> connect(), so from_raw_fd is handed a
    //    freshly-cloned /net fd that is NOT yet a connection: no peer, and
    //    fd2path says "/net/tcp/clone" with no conn number in it. std must adopt
    //    it anyway — from_raw_fd promises no validation, and being stricter than
    //    the contract rejected a legitimate caller (tokio panicked on the first
    //    connection with "not an open /net data fd").
    total += 1;
    match adopt_unconnected_socket() {
        Ok(s) => {
            println!("5 from_raw_fd adopts an unconnected socket: PASS ({s})");
            pass += 1;
        }
        Err(e) => println!("5 from_raw_fd adopts an unconnected socket: FAIL {e}"),
    }

    // 6. hostname resolution through /net/cs — only when a name to resolve is
    //    given (it needs working DNS, which the QEMU VM may not have). The
    //    /net/cs protocol reads replies from OFFSET 0 after the query write
    //    (dial(2) seeks back explicitly); reading from the current offset gets
    //    instant EOF, which made every hostname "not found" — Servo's first real
    //    site on bare metal died with "client error (Connect)" from exactly this.
    if let Some(name) = std::env::args().nth(2) {
        total += 1;
        match std::net::ToSocketAddrs::to_socket_addrs(&(name.as_str(), 80u16)) {
            Ok(addrs) => {
                let addrs: Vec<_> = addrs.collect();
                if addrs.is_empty() {
                    println!("6 resolve {name}: FAIL empty result");
                } else {
                    println!("6 resolve {name}: PASS ({} -> {})", name, addrs[0]);
                    pass += 1;
                }
            },
            Err(e) => println!("6 resolve {name}: FAIL {e}"),
        }
    }

    println!("netgate {pass}/{total} {}", if pass == total { "PASS" } else { "FAIL" });
    if pass != total {
        std::process::exit(1);
    }
}

/// Connect, hand the fd out as a bare integer, rebuild, and keep using it.
fn raw_fd_roundtrip(host: &str) -> std::io::Result<String> {
    use std::os::fd::{FromRawFd, IntoRawFd};

    let listener = TcpListener::bind((host, 0))?;
    let addr = listener.local_addr()?;
    let server = serve_once(listener);

    let stream = TcpStream::connect(addr)?;
    let peer_before = stream.peer_addr()?;

    let fd = stream.into_raw_fd();
    // SAFETY: `fd` came from into_raw_fd, so we own it and nothing else holds it.
    let mut stream = unsafe { TcpStream::from_raw_fd(fd) };

    // The rebuilt stream must still know who it is talking to, and still work.
    let peer_after = stream.peer_addr()?;
    if peer_before != peer_after {
        return Err(std::io::Error::other(format!("peer changed: {peer_before} -> {peer_after}")));
    }
    stream.write_all(b"through-a-raw-fd")?;
    let mut buf = [0u8; 32];
    let n = stream.read(&mut buf)?;
    drop(stream);
    let _ = server.join();
    if &buf[..n] != b"through-a-raw-fd" {
        return Err(std::io::Error::other("echo mismatch after round-trip"));
    }
    Ok(format!("peer {peer_after} preserved"))
}

/// The mio pattern: make a socket, adopt it, and only then dial.
fn adopt_unconnected_socket() -> std::io::Result<String> {
    use std::os::fd::{AsRawFd, FromRawFd};

    // cc9's BSD socket layer (net9.c) — the same call mio's new_socket makes.
    unsafe extern "C" {
        fn socket(domain: i32, ty: i32, protocol: i32) -> i32;
    }
    const AF_INET: i32 = 2;
    const SOCK_STREAM: i32 = 1;

    let fd = unsafe { socket(AF_INET, SOCK_STREAM, 0) };
    if fd < 0 {
        return Err(std::io::Error::last_os_error());
    }

    // Must not panic: at this point there is no connection behind the fd.
    // SAFETY: socket() just handed us this fd and nothing else owns it.
    let stream = unsafe { TcpStream::from_raw_fd(fd) };

    if stream.as_raw_fd() != fd {
        return Err(std::io::Error::other("as_raw_fd lost the fd"));
    }
    // And it must not invent a peer it does not have.
    match stream.peer_addr() {
        Ok(peer) => Err(std::io::Error::other(format!("unconnected socket claims peer {peer}"))),
        Err(_) => Ok(format!("adopted fd {fd}, no peer claimed")),
    }
}

fn serve_once(listener: TcpListener) -> thread::JoinHandle<()> {
    thread::spawn(move || {
        if let Ok((mut s, _)) = listener.accept() {
            let mut buf = [0u8; 64];
            if let Ok(n) = s.read(&mut buf) {
                let _ = s.write_all(&buf[..n]);
            }
        }
    })
}

fn echo_roundtrip(host: &str) -> std::io::Result<()> {
    let listener = TcpListener::bind((host, 0)).or_else(|_| TcpListener::bind((host, 9101)))?;
    let addr = listener.local_addr()?;
    let h = serve_once(listener);

    let mut c = TcpStream::connect(addr)?;
    c.write_all(b"hello plan9")?;
    let mut buf = [0u8; 64];
    let n = c.read(&mut buf)?;
    let _ = h.join();

    if &buf[..n] != b"hello plan9" {
        return Err(std::io::Error::other(format!("echo mismatch: {:?}", &buf[..n])));
    }
    Ok(())
}

fn addrs_readable(host: &str) -> std::io::Result<String> {
    let listener = TcpListener::bind((host, 0)).or_else(|_| TcpListener::bind((host, 9102)))?;
    let addr = listener.local_addr()?;
    let h = serve_once(listener);

    let c = TcpStream::connect(addr)?;
    // Both of these read the conn directory AFTER ctl has been dropped.
    let peer = c.peer_addr()?;
    let local = c.local_addr()?;
    drop(c);
    let _ = h.join();
    Ok(format!("peer={peer} local={local}"))
}

fn two_at_once(host: &str) -> std::io::Result<()> {
    let l1 = TcpListener::bind((host, 0)).or_else(|_| TcpListener::bind((host, 9103)))?;
    let a1 = l1.local_addr()?;
    let h1 = serve_once(l1);
    let l2 = TcpListener::bind((host, 0)).or_else(|_| TcpListener::bind((host, 9104)))?;
    let a2 = l2.local_addr()?;
    let h2 = serve_once(l2);

    let mut c1 = TcpStream::connect(a1)?;
    let mut c2 = TcpStream::connect(a2)?;
    c1.write_all(b"one")?;
    c2.write_all(b"two")?;

    let mut b1 = [0u8; 16];
    let mut b2 = [0u8; 16];
    let n1 = c1.read(&mut b1)?;
    let n2 = c2.read(&mut b2)?;
    let _ = h1.join();
    let _ = h2.join();

    if &b1[..n1] != b"one" || &b2[..n2] != b"two" {
        return Err(std::io::Error::other("crossed streams"));
    }
    Ok(())
}
