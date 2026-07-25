//! Raw mode must survive ONE signal going away while another still holds.
//!
//! The signals a child uses to announce that it drives the keyboard — the
//! alternate screen, bracketed paste, the Kitty keyboard stack — are
//! INDEPENDENT, and apps toggle them separately: nvim sits on the alternate
//! screen for its whole session and turns bracketed paste on and off around its
//! own operations. Treating any single "off" as "leave raw mode" dropped a
//! full-screen app back onto the cooked line discipline mid-session, which
//! echoed its keystrokes into the grid and swallowed its escape sequences —
//! on screen, lines duplicated with their tails missing.
//!
//! The child here does exactly that: alt screen + bracketed paste on, then
//! paste OFF while still on the alt screen, then it reports the bytes it reads.
//! Raw delivers "0d" for Enter and echoes nothing; cooked would deliver "0a"
//! (ICRNL) and echo the typed text into the grid.
//!
//! Usage: rawsticky <qjs> <script>     (the script is written by the caller)
//! Prints RAWSTICKY-OK / RAWSTICKY-FAIL.

use std::sync::Arc;
use std::time::{Duration, Instant};

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
    if argv.is_empty() {
        eprintln!("usage: rawsticky <program> [args...]");
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
    let notifier = Notifier(event_loop.channel());
    let _io_thread = event_loop.spawn();

    // The child prints READY only after it has turned bracketed paste back OFF,
    // so anything typed from here on lands in the state under test.
    let ready_deadline = Instant::now() + Duration::from_secs(60);
    loop {
        std::thread::sleep(Duration::from_millis(200));
        if grid_text(&term.lock()).contains("READY") {
            break;
        }
        if Instant::now() > ready_deadline {
            println!("RAWSTICKY-FAIL: child never announced READY");
            std::process::exit(1);
        }
    }
    std::thread::sleep(Duration::from_millis(500));
    notifier.0.send(Msg::Input(b"ab\r".as_slice().into())).expect("send keys");

    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        std::thread::sleep(Duration::from_millis(300));
        let text = grid_text(&term.lock());
        if let Some(line) = text.lines().find(|l| l.contains("GOT ")) {
            println!("--- grid ---\n{}--- end grid ---", text);
            // Raw: the three bytes arrive as typed, CR intact, nothing echoed.
            let raw_ok = line.contains("61 62 0d");
            let cooked = line.contains("0a");
            if raw_ok && !cooked {
                println!("RAWSTICKY-OK: raw survived bracketed-paste-off ({line})");
                std::process::exit(0);
            }
            println!("RAWSTICKY-FAIL: line discipline went cooked again ({line})");
            std::process::exit(1);
        }
        if Instant::now() > deadline {
            println!("--- grid (TIMEOUT) ---\n{}--- end grid ---", text);
            println!("RAWSTICKY-FAIL: child never reported the bytes it read");
            std::process::exit(1);
        }
    }
}
