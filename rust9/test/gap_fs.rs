// Gate test: fs gaps — set_permissions, set_times(mtime), canonicalize, errstr.
use std::fs;
use std::time::{Duration, UNIX_EPOCH};

fn main() {
    let dir = "/tmp/gaptest";
    let _ = fs::remove_dir_all(dir);
    fs::create_dir_all(dir).unwrap();
    let f = format!("{dir}/a.txt");
    fs::write(&f, b"hello").unwrap();

    // mtime via wstat (before perms so the file is still writable)
    let want = UNIX_EPOCH + Duration::from_secs(1_000_000_000);
    let file = fs::OpenOptions::new().write(true).open(&f).unwrap();
    file.set_times(fs::FileTimes::new().set_modified(want)).unwrap();
    drop(file);
    let got = fs::metadata(&f).unwrap().modified().unwrap();
    assert_eq!(got.duration_since(UNIX_EPOCH).unwrap().as_secs(), 1_000_000_000, "mtime mismatch");
    println!("MTIME-OK");

    // permissions via wstat chmod
    let mut p = fs::metadata(&f).unwrap().permissions();
    p.set_readonly(true);
    fs::set_permissions(&f, p.clone()).unwrap();
    assert!(fs::metadata(&f).unwrap().permissions().readonly(), "readonly not applied");
    p.set_readonly(false);
    fs::set_permissions(&f, p).unwrap();
    assert!(!fs::metadata(&f).unwrap().permissions().readonly(), "writable not restored");
    println!("PERM-OK");

    // canonicalize: absolute with ../ and ./, then relative
    let c = fs::canonicalize(format!("{dir}/../gaptest/./a.txt")).unwrap();
    assert_eq!(c, std::path::PathBuf::from(&f));
    std::env::set_current_dir(dir).unwrap();
    let c2 = fs::canonicalize("a.txt").unwrap();
    assert_eq!(c2, std::path::PathBuf::from(&f));
    assert!(fs::canonicalize("/no/such/gap").is_err(), "canonicalize must verify existence");
    println!("CANON-OK");

    // errstr fidelity: the error Display should carry the kernel string
    let e = fs::File::open("/no/such/gap").unwrap_err();
    println!("ERRSTR [{e}]");

    println!("T1-ALL-OK");
}
