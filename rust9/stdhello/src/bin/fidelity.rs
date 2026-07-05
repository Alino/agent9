//! Fidelity probes, one per arg so we can see which subsystem is faithful.
use std::sync::{Arc, Mutex};
use std::time::{Instant, SystemTime, UNIX_EPOCH};

fn main() {
    let which = std::env::args().nth(1).unwrap_or_else(|| "float".into());
    match which.as_str() {
        "float" => {
            let s = 2.0_f64.sqrt();
            let sum = (0.1_f64 + 0.2).to_string();
            println!("float: sqrt(2)={s:.15}  0.1+0.2={sum}  sin(1)={:.12}", 1.0_f64.sin());
        }
        "clock" => {
            let mut last = Instant::now();
            let mut backward = 0u32;
            for _ in 0..20_000 {
                let now = Instant::now();
                if now < last {
                    backward += 1;
                }
                last = now;
            }
            let wall = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
            println!("clock: {backward} backward Instant steps / 20k; wall epoch secs = {wall}");
        }
        "thread" => {
            // spawn+join returning values (no shared Mutex)
            let hs: Vec<_> = (0..4u64).map(|i| std::thread::spawn(move || i * i)).collect();
            let sum: u64 = hs.into_iter().map(|h| h.join().unwrap()).sum();
            println!("thread: sum of squares 0..4 across 4 threads = {sum} (correct = 14)");
        }
        "mutex" => {
            let counter = Arc::new(Mutex::new(0u64));
            let mut handles = Vec::new();
            for _ in 0..4 {
                let c = Arc::clone(&counter);
                handles.push(std::thread::spawn(move || {
                    for _ in 0..50_000 {
                        *c.lock().unwrap() += 1;
                    }
                }));
            }
            for h in handles {
                h.join().unwrap();
            }
            println!("mutex: 4x50k increments -> {} (correct = 200000)", *counter.lock().unwrap());
        }
        "contend" => {
            // Force a held-lock overlap: main holds the lock while a child tries to
            // take it. A REAL mutex makes the child block until main releases. The
            // no_threads stub asserts lock-not-held -> panics under this overlap.
            let m = Arc::new(Mutex::new(0u64));
            let m2 = Arc::clone(&m);
            let g = m.lock().unwrap();
            let t = std::thread::spawn(move || {
                let _g2 = m2.lock().unwrap();
                println!("contend: child ACQUIRED the lock (a real mutex would have blocked)");
            });
            std::thread::sleep(std::time::Duration::from_millis(200));
            println!("contend: main held the lock for 200ms, releasing now");
            drop(g);
            t.join().unwrap();
            println!("contend: done");
        }
        "mpsc" => {
            use std::sync::mpsc;
            let (tx, rx) = mpsc::channel();
            let t = std::thread::spawn(move || {
                for i in 0..5u64 {
                    std::thread::sleep(std::time::Duration::from_millis(20));
                    tx.send(i * i).unwrap();
                }
            });
            let mut sum = 0u64;
            while let Ok(v) = rx.recv() {
                sum += v;
            }
            t.join().unwrap();
            println!("mpsc: blocking-recv sum of squares 0..5 = {sum} (correct = 30)");
        }
        "rwlock" => {
            use std::sync::RwLock;
            let lock = Arc::new(RwLock::new(0u64));
            let mut hs = Vec::new();
            for _ in 0..3 {
                let l = Arc::clone(&lock);
                hs.push(std::thread::spawn(move || {
                    let g = l.read().unwrap();
                    *g
                }));
            }
            {
                let mut w = lock.write().unwrap();
                *w = 42;
            }
            let reads: u64 = hs.into_iter().map(|h| h.join().unwrap()).sum();
            println!("rwlock: writer set 42, final = {} (reads summed = {reads})", *lock.read().unwrap());
        }
        "threadstorm" => {
            // libtest spawns+joins one thread per test. Does that leak stacks/fds
            // at scale (2771 tests)? Spawn+join 5000 and watch for a stall.
            for i in 0..5000u32 {
                let h = std::thread::spawn(move || i.wrapping_mul(2));
                let _ = h.join().unwrap();
                if i % 500 == 0 {
                    println!("  spawned+joined {i}");
                }
            }
            println!("threadstorm: done 5000 spawn+join");
        }
        _ => println!("unknown probe"),
    }
}
