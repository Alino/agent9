//! Output-integrity gate: a child's lines must reach the grid exactly once, in
//! order, untruncated.
//!
//! alacritty9 reads its child through a pipe with a blocking reader thread, and
//! that read goes through cc9's poll layer. A bug there (stale ring bytes served
//! after an fd is recycled, a partial drain) shows up on screen as duplicated
//! lines with their tails missing — which is exactly what a user sees and which
//! no raw-mode test would catch. So: have the child print N numbered lines of a
//! known shape as fast as it can, then check the grid's last screenful is a
//! contiguous, strictly increasing run with no repeats and no short lines.
//!
//! Usage: linedump <program> [args...]      (child must print "NNNN <text>" lines
//!                                           and finish with "DONE")
//! Prints LINEDUMP-OK / LINEDUMP-FAIL.

use std::sync::Arc;
use std::time::{Duration, Instant};

use alacritty_terminal::event::{Event, EventListener, WindowSize};
use alacritty_terminal::event_loop::EventLoop;
use alacritty_terminal::grid::Dimensions;
use alacritty_terminal::sync::FairMutex;
use alacritty_terminal::term::test::TermSize;
use alacritty_terminal::term::{Config, Term};
use alacritty_terminal::tty::{self, Options, Shell};

#[derive(Clone)]
struct Listener;

impl EventListener for Listener {
    fn send_event(&self, _event: Event) {}
}

fn grid_text<T: EventListener>(term: &Term<T>) -> String {
    let grid = term.grid();
    let mut out = String::new();
    for line in 0..grid.screen_lines() {
        let mut row = String::new();
        for col in 0..grid.columns() {
            let point = alacritty_terminal::index::Point::new(
                alacritty_terminal::index::Line(line as i32),
                alacritty_terminal::index::Column(col),
            );
            row.push(grid[point].c);
        }
        out.push_str(row.trim_end());
        out.push('\n');
    }
    out
}

fn main() {
    tty::setup_env();

    let argv: Vec<String> = std::env::args().skip(1).collect();
    if argv.is_empty() {
        eprintln!("usage: linedump <program> [args...]");
        std::process::exit(2);
    }

    let size = TermSize::new(120, 40);
    let window_size =
        WindowSize { num_lines: 40, num_cols: 120, cell_width: 8, cell_height: 16 };

    let term = Term::new(Config::default(), &size, Listener);
    let term = Arc::new(FairMutex::new(term));

    let mut options = Options::default();
    options.shell = Some(Shell::new(argv[0].clone(), argv[1..].to_vec()));

    let pty = tty::new(&options, window_size, 0).expect("spawn child");
    let event_loop =
        EventLoop::new(term.clone(), Listener, pty, false, false).expect("event loop");
    let _io_thread = event_loop.spawn();

    let deadline = Instant::now() + Duration::from_secs(60);
    let text = loop {
        std::thread::sleep(Duration::from_millis(300));
        let text = grid_text(&term.lock());
        if text.contains("DONE") {
            break text;
        }
        if Instant::now() > deadline {
            println!("--- grid (TIMEOUT) ---\n{text}--- end grid ---");
            println!("LINEDUMP-FAIL: child never finished");
            std::process::exit(1);
        }
    };

    // Every numbered line on screen: same tail, strictly increasing, no repeats.
    let mut seen: Vec<u32> = Vec::new();
    let mut problems: Vec<String> = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line == "DONE" {
            continue;
        }
        let (num, tail) = match line.split_once(' ') {
            Some((n, t)) => (n, t),
            None => {
                problems.push(format!("malformed line {line:?}"));
                continue;
            },
        };
        match num.parse::<u32>() {
            Ok(n) => {
                if seen.contains(&n) {
                    problems.push(format!("line {n} appears more than once"));
                }
                seen.push(n);
                if tail != "the quick brown fox jumps over the lazy dog" {
                    problems.push(format!("line {n} truncated/garbled: {tail:?}"));
                }
            },
            Err(_) => problems.push(format!("unparsable line number in {line:?}")),
        }
    }
    let contiguous = seen.windows(2).all(|w| w[1] == w[0] + 1);
    if !contiguous {
        problems.push(format!("line numbers are not contiguous: {seen:?}"));
    }

    println!("--- grid ---\n{text}--- end grid ---");
    if problems.is_empty() && seen.len() >= 10 {
        println!("LINEDUMP-OK: {} lines, in order, intact", seen.len());
        std::process::exit(0);
    }
    for p in &problems {
        println!("  {p}");
    }
    println!("LINEDUMP-FAIL: {} problems over {} lines", problems.len(), seen.len());
    std::process::exit(1);
}
