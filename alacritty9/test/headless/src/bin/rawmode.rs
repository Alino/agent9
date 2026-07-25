//! Raw-mode gate: prove a keystroke-driven app that renders INLINE (no
//! alternate screen) gets its keys through tty/plan9.rs.
//!
//! Cooked mode buffers a whole line and turns Enter into \n, so such an app
//! never sees an Enter keypress at all — pi showed the typed text and did
//! nothing. tty/plan9.rs therefore switches to raw on bracketed paste and the
//! Kitty keyboard push as well as on the alternate screen. This drives the
//! real reactor with no display: spawn the app, send text + Enter (\r, what
//! the terminal core sends), and look for the answer in the grid.
//!
//! Usage: rawmode <program> [args...] -- <ready marker> <text to type> <expect>
//!
//! It waits for <ready marker> to appear in the grid before typing: raw mode
//! only engages once the app has announced itself, and typing before that goes
//! through the cooked path (which is exactly the bug being tested for).
//! Prints RAWMODE-OK / RAWMODE-FAIL.

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

    // rawmode <program> [args...] -- <type> <expect>
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let split = argv.iter().position(|a| a == "--").unwrap_or_else(|| {
        eprintln!("usage: rawmode <program> [args...] -- <text> <expect>");
        std::process::exit(2);
    });
    let program = argv[0].clone();
    let args: Vec<String> = argv[1..split].to_vec();
    let ready = argv[split + 1].clone();
    let typed = argv[split + 2].clone();
    let expect = argv[split + 3].clone();

    let size = TermSize::new(120, 40);
    let window_size =
        WindowSize { num_lines: 40, num_cols: 120, cell_width: 8, cell_height: 16 };

    let term = Term::new(Config::default(), &size, Listener);
    let term = Arc::new(FairMutex::new(term));

    let mut options = Options::default();
    options.shell = Some(Shell::new(program, args));

    let pty = tty::new(&options, window_size, 0).expect("spawn child");
    let event_loop =
        EventLoop::new(term.clone(), Listener, pty, false, false).expect("event loop");
    let notifier = Notifier(event_loop.channel());
    let _io_thread = event_loop.spawn();

    // Wait for the app to be up (its announcement is what flips the line
    // discipline into raw mode), then give it a beat before typing.
    let ready_deadline = Instant::now() + Duration::from_secs(60);
    loop {
        std::thread::sleep(Duration::from_millis(250));
        if grid_text(&term.lock()).contains(&ready) {
            break;
        }
        if Instant::now() > ready_deadline {
            println!("RAWMODE-FAIL: ready marker {:?} never appeared", ready);
            std::process::exit(1);
        }
    }
    std::thread::sleep(Duration::from_millis(1500));
    notifier.0.send(Msg::Input(typed.into_bytes().into())).expect("send text");
    std::thread::sleep(Duration::from_millis(300));
    notifier.0.send(Msg::Input(b"\r".as_slice().into())).expect("send enter");

    let deadline = Instant::now() + Duration::from_secs(90);
    loop {
        std::thread::sleep(Duration::from_millis(500));
        let text = grid_text(&term.lock());
        if text.contains(&expect) {
            println!("--- grid ---\n{}--- end grid ---", text);
            println!("RAWMODE-OK: found {:?} in the grid", expect);
            std::process::exit(0);
        }
        if Instant::now() > deadline {
            println!("--- grid (TIMEOUT) ---\n{}--- end grid ---", text);
            println!("RAWMODE-FAIL: {:?} never appeared", expect);
            std::process::exit(1);
        }
    }
}
