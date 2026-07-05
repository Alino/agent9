use std::fs;
use std::io::Write;
use std::time::Instant;

fn main() {
    let t0 = Instant::now();

    let args: Vec<String> = std::env::args().collect();
    println!("args: {args:?}");
    println!("$home = {}", std::env::var("home").unwrap_or_else(|_| "<unset>".into()));

    // fs: write a file, then read it back + stat it
    let path = "/tmp/rust9_demo.txt";
    {
        let mut f = fs::File::create(path).unwrap();
        writeln!(f, "written by Rust std on 9front").unwrap();
    }
    let contents = fs::read_to_string(path).unwrap();
    print!("read back: {contents}");
    println!("file size: {} bytes", fs::metadata(path).unwrap().len());

    // fs: walk a directory
    let mut names: Vec<String> = fs::read_dir("/tmp")
        .unwrap()
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    println!("/tmp has {} entries; first few: {:?}", names.len(), &names[..names.len().min(5)]);

    // real thread over cc9 pthreads
    let sum = std::thread::spawn(|| (1..=100u64).sum::<u64>()).join().unwrap();
    println!("thread sum 1..=100 = {sum}");

    println!("elapsed: {} us", t0.elapsed().as_micros());
    let _ = fs::remove_file(path);
}
