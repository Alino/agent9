// Gate test: UDP over /net/udp (headers mode) + TTL. Arg 1 = the box's own IP
// (loopback is unconfigured on cirno; datagrams go via the real interface).
use std::net::{SocketAddr, UdpSocket};

fn main() {
    let ip = std::env::args().nth(1).unwrap_or_else(|| "192.168.88.159".into());
    let a = UdpSocket::bind("0.0.0.0:17098").unwrap();
    let b = UdpSocket::bind("0.0.0.0:17099").unwrap();
    println!("BOUND a={:?} b={:?}", a.local_addr(), b.local_addr());

    b.set_ttl(64).unwrap();
    assert_eq!(b.ttl().unwrap(), 64);
    println!("TTL-OK");

    let dst: SocketAddr = format!("{ip}:17098").parse().unwrap();
    b.send_to(b"ping-udp", dst).unwrap();
    let mut buf = [0u8; 128];
    let (n, from) = a.recv_from(&mut buf).unwrap();
    println!("RECV [{}] from {}", std::str::from_utf8(&buf[..n]).unwrap(), from);
    assert_eq!(&buf[..n], b"ping-udp");

    // reply to the parsed source address (proves the Udphdr raddr/rport decode)
    a.send_to(b"pong-udp", from).unwrap();
    let (n2, _) = b.recv_from(&mut buf).unwrap();
    assert_eq!(&buf[..n2], b"pong-udp");
    println!("ROUNDTRIP-OK");

    // connected-socket send/recv
    b.connect(dst).unwrap();
    b.send(b"c1").unwrap();
    let (n3, from3) = a.recv_from(&mut buf).unwrap();
    assert_eq!(&buf[..n3], b"c1");
    a.send_to(b"c2", from3).unwrap();
    let n4 = b.recv(&mut buf).unwrap();
    assert_eq!(&buf[..n4], b"c2");
    println!("CONNECTED-OK");

    println!("T3-ALL-OK");
}
