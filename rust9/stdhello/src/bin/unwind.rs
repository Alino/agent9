//! Unwinding probes, incl. across a spawned-thread boundary (what libtest does).
use std::panic;

struct LoudDrop(&'static str);
impl Drop for LoudDrop {
    fn drop(&mut self) {
        println!("  drop ran during unwind: {}", self.0);
    }
}

fn main() {
    match std::env::args().nth(1).as_deref().unwrap_or("main") {
        "main" => {
            let r = panic::catch_unwind(|| {
                let _g = LoudDrop("raii");
                panic!("boom from main");
            });
            println!("main: caught = {}", r.is_err());
        }
        "in_thread" => {
            // catch_unwind INSIDE a spawned thread (cc9 pthread stack)
            let h = std::thread::spawn(|| {
                let r = panic::catch_unwind(|| {
                    let _g = LoudDrop("thread-raii");
                    panic!("boom in thread");
                });
                println!("  in-thread caught = {}", r.is_err());
            });
            h.join().unwrap();
            println!("in_thread: joined ok");
        }
        "join_panic" => {
            // a thread whose panic propagates to join() -> Err. This is EXACTLY
            // what libtest relies on to report a failing test.
            let h = std::thread::spawn(|| {
                let _g = LoudDrop("join-raii");
                panic!("boom, joined");
            });
            let r = h.join();
            println!("join_panic: join is_err = {}", r.is_err());
        }
        other => println!("unknown probe: {other}"),
    }
}
