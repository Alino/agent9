//! Grid dumper: run a real TUI (nvim, pi, ...) through the tty layer with NO
//! display, type keys into it, and print the grid the terminal core holds.
//!
//! This separates the two halves of a "the screen looks wrong" report: anything
//! wrong HERE is the terminal core or the line discipline; anything wrong only on
//! the physical screen is the gl9win2/GL presenter, which this never touches.
//!
//! Usage: tuidump <settle-secs> <text-to-type> <program> [args...]

use std::sync::Arc;
use std::time::Duration;

use alacritty_terminal::event::{Event, EventListener, WindowSize};
use alacritty_terminal::event_loop::{EventLoop, Msg, Notifier};
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
    if argv.len() < 3 {
        eprintln!("usage: tuidump <settle-secs> <text> <program> [args...]");
        std::process::exit(2);
    }
    let settle: u64 = argv[0].parse().unwrap_or(5);
    let typed = argv[1].clone();

    let size = TermSize::new(100, 30);
    let window_size =
        WindowSize { num_lines: 30, num_cols: 100, cell_width: 8, cell_height: 16 };

    let term = Term::new(Config::default(), &size, Listener);
    let term = Arc::new(FairMutex::new(term));

    let mut options = Options::default();
    options.shell = Some(Shell::new(argv[2].clone(), argv[3..].to_vec()));

    let pty = tty::new(&options, window_size, 0).expect("spawn child");
    let event_loop =
        EventLoop::new(term.clone(), Listener, pty, false, false).expect("event loop");
    let notifier = Notifier(event_loop.channel());
    let _io_thread = event_loop.spawn();

    std::thread::sleep(Duration::from_secs(settle));
    println!("=== grid after startup ===\n{}", grid_text(&term.lock()));

    if !typed.is_empty() && typed != "-" {
        for chunk in typed.split('|') {
            let bytes: Vec<u8> = if let Some(hex) = chunk.strip_prefix("0x") {
                hex.as_bytes()
                    .chunks(2)
                    .filter_map(|p| u8::from_str_radix(std::str::from_utf8(p).ok()?, 16).ok())
                    .collect()
            } else {
                chunk.as_bytes().to_vec()
            };
            notifier.0.send(Msg::Input(bytes.into())).expect("send keys");
            std::thread::sleep(Duration::from_millis(400));
        }
        let after: u64 = std::env::var("TUIDUMP_AFTER").ok().and_then(|v| v.parse().ok()).unwrap_or(2);
        std::thread::sleep(Duration::from_secs(after));
        println!("=== grid after typing ===\n{}", grid_text(&term.lock()));
    }
}
