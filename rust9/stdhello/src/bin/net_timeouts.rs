//! net_timeouts.rs — verifies N1 (connect_timeout) and N2 (read timeout) on 9front.
//! Needs a hold-open TCP server on the dev host at 192.168.88.10:18080 (accepts and
//! never sends) so the read blocks until the timeout fires.
use std::io::Read;
use std::net::{SocketAddr, TcpStream};
use std::time::{Duration, Instant};

fn main() {
    let mut pass = 0u32;
    let mut fail = 0u32;
    macro_rules! check {
        ($n:expr, $c:expr) => {
            if $c {
                pass += 1;
                println!("PASS  {}", $n);
            } else {
                fail += 1;
                println!("FAIL  {}", $n);
            }
        };
    }

    // N2 — set_read_timeout then a read that has no data must return TimedOut at
    // ~the deadline, not block forever (was Err(Unsupported), silently ignored).
    match TcpStream::connect("192.168.88.10:18080") {
        Ok(mut s) => {
            s.set_read_timeout(Some(Duration::from_millis(1000))).unwrap();
            let got = s.read_timeout().unwrap();
            println!("  N2 read_timeout() = {got:?}");
            check!("N2 getter round-trips", got == Some(Duration::from_millis(1000)));
            let t0 = Instant::now();
            let mut buf = [0u8; 64];
            let r = s.read(&mut buf);
            let el = t0.elapsed();
            println!("  N2 read -> {:?} after {}ms", r.as_ref().map(|_| ()).map_err(|e| e.kind()), el.as_millis());
            check!("N2 read times out (not blocks/errors)", matches!(&r, Err(e) if e.kind() == std::io::ErrorKind::TimedOut));
            check!("N2 fires at ~1s", el >= Duration::from_millis(700) && el <= Duration::from_millis(2500));
        }
        Err(e) => println!("  N2 SKIP: couldn't reach hold-server: {e}"),
    }

    // N1 — connect_timeout to a black-hole (TEST-NET 192.0.2.1, RFC5737) must return
    // BOUNDED (~2s), not hang through the whole TCP handshake for minutes.
    let addr: SocketAddr = "192.0.2.1:80".parse().unwrap();
    let t0 = Instant::now();
    let r = TcpStream::connect_timeout(&addr, Duration::from_millis(2000));
    let el = t0.elapsed();
    println!("  N1 connect_timeout -> {:?} after {}ms", r.as_ref().map(|_| ()).map_err(|e| e.kind()), el.as_millis());
    // Either TimedOut (SYN black-holed) or a fast route error (gateway rejected) is
    // fine — both mean we honored the deadline instead of blocking for minutes.
    check!("N1 connect_timeout is bounded (<= ~3.5s)", el <= Duration::from_millis(3500));

    println!("---- {pass} passed, {fail} failed");
    std::process::exit(if fail == 0 { 0 } else { 1 });
}
