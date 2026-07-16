//! parity_gates.rs — verifies the rust9 parity fixes land at RUNTIME on 9front.
//! Build for x86_64-unknown-plan9, run on the box. Expects "N passed, 0 failed".
use std::env;
use std::fs::File;
use std::io::Write;
use std::process::Command;

fn main() {
    let mut pass = 0u32;
    let mut fail = 0u32;
    macro_rules! check {
        ($name:expr, $cond:expr) => {
            if $cond {
                pass += 1;
                println!("PASS  {}", $name);
            } else {
                fail += 1;
                println!("FAIL  {}", $name);
            }
        };
    }

    // F2 — sync_all/sync_data now call cc9 fsync/fdatasync (real durability on gefs),
    // not the old fake Ok(()) that silently lost data after a "synced" commit.
    {
        let mut f = File::create("/tmp/pg_fsync").unwrap();
        f.write_all(b"durable").unwrap();
        let a = f.sync_all();
        let d = f.sync_data();
        println!("  F2 sync_all={a:?} sync_data={d:?}");
        check!("F2 fsync file -> Ok", a.is_ok() && d.is_ok());
    }

    // F5 — try_lock must be HONEST (Err/Unsupported), not the old fake Ok that made
    // single-instance guards + cache locks think they held a lock they didn't.
    {
        let f = File::create("/tmp/pg_lock").unwrap();
        let r = f.try_lock();
        println!("  F5 try_lock={r:?}");
        check!("F5 try_lock -> Err (not fake Ok)", r.is_err());
    }

    // F9 — current_exe via /proc/$pid/text + cc9_fd2path (was Unsupported).
    {
        let e = env::current_exe();
        println!("  F9 current_exe={e:?}");
        check!(
            "F9 current_exe -> real path",
            e.as_ref().map(|p| !p.as_os_str().is_empty()).unwrap_or(false)
        );
    }

    // F10 — home_dir == $home (was None).
    {
        let h = env::home_dir();
        let want = env::var_os("home");
        println!("  F10 home_dir={h:?} $home={want:?}");
        check!(
            "F10 home_dir == $home",
            h.is_some() && h.as_ref().map(|p| p.as_os_str().to_owned()) == want
        );
    }

    // F3 — Command::env must NOT leak the child's var back into the PARENT's env.
    // The fix rforks the child with RFENVG (private env group) before setenv'ing the
    // deltas; without it the child shared /env and polluted the parent.
    {
        let key = "PG_CHILD_ONLY_9";
        env::remove_var(key); // clean start
        let _ = Command::new("/bin/echo").env(key, "in-child").arg("").status();
        let leaked = env::var(key).is_ok();
        println!("  F3 parent sees child's env var? {leaked}");
        check!("F3 no env leak into parent", !leaked);
    }

    // F4 — env::set_var must be visible to env::vars() (env() now readdirs /env live,
    // instead of walking the stale startup `environ` snapshot).
    {
        let key = "PG_SETVAR_9";
        env::set_var(key, "seen");
        let via_var = env::var(key).ok();
        let via_vars = env::vars().find(|(k, _)| k == key).map(|(_, v)| v);
        println!("  F4 var()={via_var:?} vars()={via_vars:?}");
        check!(
            "F4 set_var visible to vars()",
            via_var.as_deref() == Some("seen") && via_vars.as_deref() == Some("seen")
        );
        env::remove_var(key);
    }

    // F11 — env_clear() must give the child an EMPTY env (private group via RFCENVG),
    // AND explicitly-set vars must STILL populate. The historical cc9 note claimed
    // setenv-after-RFCENVG didn't take (deltas vanish); the fix asserts it does.
    // Verify BOTH on-box: child's own var present, parent's var gone.
    {
        env::set_var("PG_PARENT_CLR", "PARENTVAL");
        // Read the child's /env directly with `cat` (needs no env of its own, unlike
        // rc whose rcmain dies on an empty $path). /env/NAME is where setenv writes.
        // F11a: setenv AFTER RFCENVG must populate the child's /env (historical cc9 bug
        // was that the deltas vanished).
        let set_s = Command::new("/bin/cat")
            .arg("/env/PG_CHILD_CLR")
            .env_clear()
            .env("PG_CHILD_CLR", "CHILDVAL")
            .output()
            .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
            .unwrap_or_default();
        println!("  F11a child /env/PG_CHILD_CLR = {:?}", set_s.trim());
        check!("F11 env_clear: child's set var populates (setenv after RFCENVG)", set_s.contains("CHILDVAL"));
        // F11b: the parent's var must NOT appear in the cleared child's /env.
        let clr_s = Command::new("/bin/cat")
            .arg("/env/PG_PARENT_CLR")
            .env_clear()
            .env("PG_CHILD_CLR", "CHILDVAL")
            .output()
            .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
            .unwrap_or_default();
        println!("  F11b child /env/PG_PARENT_CLR = {:?} (want empty)", clr_s.trim());
        check!("F11 env_clear: parent env cleared", !clr_s.contains("PARENTVAL"));
        env::remove_var("PG_PARENT_CLR");
    }

    println!("----\n{pass} passed, {fail} failed");
    std::process::exit(if fail == 0 { 0 } else { 1 });
}
