// Gate test: process gaps — try_wait (running + exited) and the output()
// both-streams-large deadlock (sequential drain would wedge on the pipe buffer).
use std::io::Write;
use std::process::Command;

const CHUNK: usize = 1024;
const CHUNKS: usize = 200;

fn main() {
    let mut args = std::env::args();
    let me = args.next().unwrap();
    if args.next().as_deref() == Some("child") {
        let stdout = std::io::stdout();
        let stderr = std::io::stderr();
        let mut o = stdout.lock();
        let mut e = stderr.lock();
        let chunk = [b'x'; CHUNK];
        for _ in 0..CHUNKS {
            o.write_all(&chunk).unwrap();
            e.write_all(&chunk).unwrap();
        }
        return;
    }

    // try_wait on a live child
    let mut c = Command::new("/bin/sleep").arg("3").spawn().unwrap();
    match c.try_wait().unwrap() {
        None => println!("TRYWAIT-RUNNING-OK"),
        Some(s) => println!("TRYWAIT-RUNNING-FAIL {s}"),
    }
    let s = c.wait().unwrap();
    println!("WAIT [{s}]");

    // try_wait on an already-exited, never-waited child
    let mut c2 = Command::new("/bin/echo").arg("hi").stdout(std::process::Stdio::null()).spawn().unwrap();
    std::thread::sleep(std::time::Duration::from_millis(1500));
    match c2.try_wait().unwrap() {
        Some(s) => println!("TRYWAIT-EXITED-OK [{s}]"),
        None => println!("TRYWAIT-EXITED-FAIL"),
    }

    // output(): child floods BOTH streams; must not deadlock
    let out = Command::new(&me).arg("child").output().unwrap();
    println!("OUTPUT stdout={} stderr={}", out.stdout.len(), out.stderr.len());
    assert_eq!(out.stdout.len(), CHUNK * CHUNKS);
    assert_eq!(out.stderr.len(), CHUNK * CHUNKS);
    println!("T2-ALL-OK");
}
