//! Real std::process::Command on 9front (rfork+exec+await + pipes via cc9).
use std::process::Command;

fn main() {
    // 1. spawn with inherited stdio + wait
    print!("inherited child says: ");
    let s1 = Command::new("/bin/echo").arg("hello from a Rust-spawned child").status();
    println!("  echo status -> {s1:?}");

    // 2. capture a child's stdout via a pipe (Command::output) — what rustc uses
    //    to read the linker's diagnostics.
    match Command::new("/bin/echo").arg("captured-through-a-pipe").output() {
        Ok(o) => println!(
            "output(): status={:?} stdout={:?}",
            o.status.code(),
            String::from_utf8_lossy(&o.stdout).trim_end()
        ),
        Err(e) => println!("output() error: {e}"),
    }

    // 3. a real 9front tool: `cat` a file through a pipe
    match Command::new("/bin/cat").arg("/dev/user").output() {
        Ok(o) => println!("cat /dev/user -> {:?}", String::from_utf8_lossy(&o.stdout).trim_end()),
        Err(e) => println!("cat error: {e}"),
    }

    println!("getpid() = {}", std::process::id());
}
