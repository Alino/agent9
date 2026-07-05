//! rgrep — a recursive regex grep for 9front, built on the real `regex` crate.
//! Flagship for rust9: exercises std (fs walk, File I/O, args, process::exit) +
//! a heavy crates.io dependency, all cross-compiled and run on stock 9front.
use regex::Regex;
use std::env;
use std::fs;
use std::io::{self, BufWriter, Read, Write};
use std::path::Path;

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("usage: {} PATTERN [PATH]", args.first().map(String::as_str).unwrap_or("rgrep"));
        std::process::exit(2);
    }
    let re = match Regex::new(&args[1]) {
        Ok(re) => re,
        Err(e) => {
            eprintln!("rgrep: bad pattern: {e}");
            std::process::exit(2);
        }
    };
    let root = args.get(2).map(String::as_str).unwrap_or(".");

    let stdout = io::stdout();
    let mut out = BufWriter::new(stdout.lock());
    let mut matches = 0u64;
    let mut files = 0u64;
    walk(Path::new(root), &re, &mut out, &mut matches, &mut files);
    let _ = out.flush();
    eprintln!("rgrep: {matches} match(es) across {files} file(s)");
}

fn walk(path: &Path, re: &Regex, out: &mut impl Write, matches: &mut u64, files: &mut u64) {
    let md = match fs::metadata(path) {
        Ok(m) => m,
        Err(_) => return,
    };
    if md.is_dir() {
        let Ok(entries) = fs::read_dir(path) else { return };
        let mut children: Vec<_> = entries.filter_map(Result::ok).map(|e| e.path()).collect();
        children.sort();
        for child in children {
            walk(&child, re, out, matches, files);
        }
    } else if md.is_file() {
        grep_file(path, re, out, matches, files);
    }
}

fn grep_file(path: &Path, re: &Regex, out: &mut impl Write, matches: &mut u64, files: &mut u64) {
    let Ok(mut f) = fs::File::open(path) else { return };
    let mut buf = Vec::new();
    if f.read_to_end(&mut buf).is_err() {
        return;
    }
    // Skip binaries: a NUL byte in the first 8 KiB is a good-enough heuristic.
    if buf.iter().take(8192).any(|&b| b == 0) {
        return;
    }
    *files += 1;
    let text = String::from_utf8_lossy(&buf);
    for (n, line) in text.lines().enumerate() {
        if re.is_match(line) {
            let _ = writeln!(out, "{}:{}:{}", path.display(), n + 1, line);
            *matches += 1;
        }
    }
}
