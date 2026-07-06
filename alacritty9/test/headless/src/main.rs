//! P1 gate: prove alacritty_terminal works end-to-end on 9front with no
//! display — spawn rc through tty/plan9.rs, feed it commands through the
//! real reactor (event_loop.rs over the polling shim), and dump the grid.
//!
//! PASS criteria printed as GATE1-OK; any panic/timeout is a fail.

use std::sync::Arc;
use std::time::{Duration, Instant};

use alacritty_terminal::event::{Event, EventListener, WindowSize};
use alacritty_terminal::event_loop::{EventLoop, Msg, Notifier};
use alacritty_terminal::grid::Dimensions;
use alacritty_terminal::sync::FairMutex;
use alacritty_terminal::term::test::TermSize;
use alacritty_terminal::term::{Config, Term};
use alacritty_terminal::tty::{self, Options};

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
            let point =
                alacritty_terminal::index::Point::new(alacritty_terminal::index::Line(line as i32), alacritty_terminal::index::Column(col));
            row.push(grid[point].c);
        }
        out.push_str(row.trim_end());
        out.push('\n');
    }
    out
}

fn main() {
    tty::setup_env();

    let size = TermSize::new(80, 24);
    let window_size =
        WindowSize { num_lines: 24, num_cols: 80, cell_width: 8, cell_height: 16 };

    let term = Term::new(Config::default(), &size, Listener);
    let term = Arc::new(FairMutex::new(term));

    let pty = tty::new(&Options::default(), window_size, 0).expect("spawn rc");
    let event_loop =
        EventLoop::new(term.clone(), Listener, pty, false, false).expect("event loop");
    let notifier = Notifier(event_loop.channel());
    let _io_thread = event_loop.spawn();

    // Give rc a moment to start, then run a command.
    std::thread::sleep(Duration::from_millis(300));
    notifier.0.send(Msg::Input("echo GATE1-`{echo OK}\n".as_bytes().into())).expect("send input");

    // Poll the grid until the output shows up (rc echoes through the whole
    // reactor: pipes -> reader thread -> polling shim -> vte -> grid).
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        std::thread::sleep(Duration::from_millis(200));
        let text = grid_text(&term.lock());
        if text.contains("GATE1-OK") {
            println!("--- grid dump ---\n{}--- end grid ---", text);
            println!("GATE1-OK: alacritty_terminal headless on plan9 works");
            std::process::exit(0);
        }
        if Instant::now() > deadline {
            println!("--- grid dump (TIMEOUT) ---\n{}--- end grid ---", text);
            println!("GATE1-FAIL: expected output never reached the grid");
            std::process::exit(1);
        }
    }
}
