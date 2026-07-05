// Gate test: per-thread errno. Two threads hammer failing syscalls with
// DIFFERENT errnos (ENOENT vs EEXIST); with a shared global errno the kinds
// cross-contaminate, with per-thread slots there are zero leaks.
use std::io::ErrorKind;
use std::sync::atomic::{AtomicUsize, Ordering};

static BAD: AtomicUsize = AtomicUsize::new(0);
const ITERS: usize = 20_000;

fn main() {
    let t1 = std::thread::spawn(|| {
        for _ in 0..ITERS {
            let e = std::fs::metadata("/no/such/file/gap").unwrap_err();
            if e.kind() != ErrorKind::NotFound {
                BAD.fetch_add(1, Ordering::Relaxed);
            }
        }
    });
    let t2 = std::thread::spawn(|| {
        for _ in 0..ITERS {
            let e = std::fs::create_dir("/tmp").unwrap_err();
            if e.kind() != ErrorKind::AlreadyExists {
                BAD.fetch_add(1, Ordering::Relaxed);
            }
        }
    });
    t1.join().unwrap();
    t2.join().unwrap();
    let bad = BAD.load(Ordering::Relaxed);
    println!("ERRNO-CROSS-LEAKS {bad}");
    if bad == 0 {
        println!("T4-ALL-OK");
    }
}
