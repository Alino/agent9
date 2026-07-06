//! Plan 9 "PTY".
//!
//! Plan 9 has no pseudoterminals, termios, or controlling ttys. The native
//! model (9term, acme win) is: run `rc -i` with its fds on plain pipes; the
//! terminal does the echo/line-editing it wants. This module implements
//! alacritty_terminal's PTY traits over exactly that: a child spawned with
//! std::process pipes, one blocking reader thread per output pipe (the Plan 9
//! substitute for poll — see the polling shim), and stdout-EOF standing in
//! for SIGCHLD.

use std::collections::VecDeque;
use std::io::{self, Read};
use std::process::{Child, ChildStdin, Command, ExitStatus, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use polling::{Event, PollMode, Poller};

use crate::event::{OnResize, WindowSize};
use crate::tty::{ChildEvent, EventedPty, EventedReadWrite, Options};

pub(crate) const PTY_READ_WRITE_TOKEN: usize = 0;
pub(crate) const PTY_CHILD_EVENT_TOKEN: usize = 1;

/// State shared between the reader threads and the `Pty`.
///
/// Lock order (both are taken only in this order): `buf` → poller internals.
/// Sticky readiness is cleared only while `buf`'s lock proves it empty, so a
/// concurrent push can't be lost.
struct Shared {
    buf: Mutex<VecDeque<u8>>,
    poller: Mutex<Option<Arc<Poller>>>,
    eof: AtomicBool,
    /// Raw mode, keyed on the child entering/leaving the alternate screen
    /// (ESC[?1049h/l, ESC[?47h/l). There is no pty/termios to ask for raw
    /// mode on Plan 9, but every full-screen TUI announces itself this way;
    /// while set, the line discipline is bypassed entirely (no echo, no
    /// line buffer, no ICRNL/ONLCR, escape sequences and ^C pass through).
    raw: AtomicBool,
}

impl Shared {
    fn wake_read(&self) {
        if let Some(poller) = self.poller.lock().unwrap().as_ref() {
            poller.shim_set_ready_read(PTY_READ_WRITE_TOKEN);
        }
    }

    fn wake_child(&self) {
        if let Some(poller) = self.poller.lock().unwrap().as_ref() {
            poller.shim_set_ready_read(PTY_CHILD_EVENT_TOKEN);
        }
    }
}

/// Alt-screen enter/leave sequences that toggle raw mode. Matched with a
/// rolling tail so a sequence split across two reads is still seen.
const RAW_ON: [&[u8]; 2] = [b"\x1b[?1049h", b"\x1b[?47h"];
const RAW_OFF: [&[u8]; 2] = [b"\x1b[?1049l", b"\x1b[?47l"];

fn spawn_reader(shared: Arc<Shared>, mut pipe: impl Read + Send + 'static, signal_eof: bool) {
    std::thread::Builder::new()
        .name("plan9 pty reader".into())
        .spawn(move || {
            let mut chunk = [0u8; 0x1_0000];
            let mut last = 0u8;
            let mut tail: VecDeque<u8> = VecDeque::with_capacity(16);
            loop {
                match pipe.read(&mut chunk) {
                    Ok(0) | Err(_) => break,
                    Ok(n) => {
                        let mut buf = shared.buf.lock().unwrap();
                        for &byte in &chunk[..n] {
                            // Track alt-screen toggles for raw mode.
                            if tail.len() == 16 {
                                tail.pop_front();
                            }
                            tail.push_back(byte);
                            let ends_with = |pat: &[u8]| {
                                tail.len() >= pat.len()
                                    && tail.iter().rev().zip(pat.iter().rev()).all(|(a, b)| a == b)
                            };
                            if RAW_ON.iter().any(|p| ends_with(p)) {
                                shared.raw.store(true, Ordering::Release);
                            } else if RAW_OFF.iter().any(|p| ends_with(p)) {
                                shared.raw.store(false, Ordering::Release);
                            }

                            // ONLCR: children on a pipe emit bare \n; a pty
                            // would translate for the terminal, so we do
                            // (else output stair-steps). Off in raw mode —
                            // full-screen apps position explicitly.
                            if byte == b'\n'
                                && last != b'\r'
                                && !shared.raw.load(Ordering::Acquire)
                            {
                                buf.push_back(b'\r');
                            }
                            buf.push_back(byte);
                            last = byte;
                        }
                        shared.wake_read();
                    },
                }
            }
            if signal_eof {
                shared.eof.store(true, Ordering::Release);
                shared.wake_child();
            }
        })
        .expect("spawn pty reader thread");
}

pub struct Pty {
    child: Child,
    reader: PtyReader,
    writer: LineDiscipline,
    shared: Arc<Shared>,
}

/// The minimal canonical-mode line discipline (9term's job on Plan 9, a
/// pty's on unix): local ECHO into the terminal, line buffering with
/// backspace erase, Enter (\r) -> \n (ICRNL), ^C -> "interrupt" note to the
/// child's note group.
///
/// ponytail: no raw-mode switch — a piped child can't request one (that
/// needs /dev/consctl, which only real cons/9term namespaces have), so
/// full-screen apps are out of scope; rc/sam -d/cat-class programs work.
pub struct LineDiscipline {
    stdin: ChildStdin,
    line: Vec<u8>,
    shared: Arc<Shared>,
    child_pid: u32,
}

impl LineDiscipline {
    /// Echo bytes into the terminal by feeding them to the reader buffer, as
    /// if the child had written them.
    fn echo(&self, bytes: &[u8]) {
        let mut buf = self.shared.buf.lock().unwrap();
        buf.extend(bytes);
        self.shared.wake_read();
    }

    fn interrupt(&self) {
        let _ = std::fs::write(format!("/proc/{}/notepg", self.child_pid), "interrupt");
    }
}

impl io::Write for LineDiscipline {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        use std::io::Write as _;

        // Raw mode (child is on the alternate screen): the child owns the
        // byte stream — keystrokes, escape sequences, mouse reports, ^C all
        // pass through verbatim, no echo, no line buffer.
        if self.shared.raw.load(Ordering::Acquire) {
            self.line.clear();
            self.stdin.write_all(data)?;
            self.stdin.flush()?;
            return Ok(data.len());
        }

        for &byte in data {
            match byte {
                0x03 => {
                    // ^C: interrupt the child (Plan 9 note), drop the line.
                    self.line.clear();
                    self.echo(b"\r\n");
                    self.interrupt();
                },
                0x7f | 0x08 => {
                    if self.line.pop().is_some() {
                        self.echo(b"\x08 \x08");
                    }
                },
                b'\r' | b'\n' => {
                    self.echo(b"\r\n");
                    self.line.push(b'\n');
                    self.stdin.write_all(&self.line)?;
                    self.stdin.flush()?;
                    self.line.clear();
                },
                _ => {
                    // Printables and UTF-8 continuation bytes; other control
                    // bytes (escape sequences from arrow keys etc.) are
                    // meaningless to a piped child — drop them.
                    if byte >= 0x20 || byte == b'\t' {
                        self.line.push(byte);
                        self.echo(&[byte]);
                    }
                },
            }
        }
        Ok(data.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        self.stdin.flush()
    }
}

/// Publish the terminal size where piped children can see it. Plan 9 has no
/// TIOCSWINSZ, but the child shares our env GROUP (no RFENVG at spawn), so
/// /env/LINES and /env/COLS are live files it can re-read — pi9's bubbletea
/// polls them the way it polls the vts size file.
fn publish_size(window_size: WindowSize) {
    // Single-threaded env access isn't a real concern on Plan 9 (env vars
    // are per-group /env files), and both callers run on the same thread.
    unsafe {
        std::env::set_var("LINES", window_size.num_lines.to_string());
        std::env::set_var("COLS", window_size.num_cols.to_string());
    }
}

pub fn new(config: &Options, window_size: WindowSize, _window_id: u64) -> io::Result<Pty> {
    publish_size(window_size);
    let (program, args) = match &config.shell {
        Some(shell) => (shell.program.as_str(), shell.args.as_slice()),
        // The 9term/win default: interactive rc even though stdin is a pipe.
        None => ("/bin/rc", &["-i".to_string()][..]),
    };

    // Two pipes would let the child's stdout and stderr race each other into
    // the grid (rc's prompt is stderr, command output stdout). Merge them
    // inside the child with an rc-level >[2=1] so one pipe carries both, in
    // order. `rfork s` gives the session its own NOTE GROUP — without it the
    // child shares ours, and the ^C "interrupt" notepg would kill Alacritty
    // itself.
    let mut cmdline = String::from("rfork s; exec ");
    for word in std::iter::once(&program.to_string()).chain(args.iter()) {
        cmdline.push('\'');
        cmdline.push_str(&word.replace('\'', "''"));
        cmdline.push_str("' ");
    }
    cmdline.push_str(">[2=1]");

    let mut builder = Command::new("/bin/rc");
    builder.args(["-c", &cmdline]);
    builder.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped());

    if let Some(dir) = &config.working_directory {
        builder.current_dir(dir);
    }
    for (key, value) in &config.env {
        builder.env(key, value);
    }

    let mut child = builder.spawn()?;

    let shared = Arc::new(Shared {
        buf: Mutex::new(VecDeque::new()),
        poller: Mutex::new(None),
        eof: AtomicBool::new(false),
        raw: AtomicBool::new(false),
    });

    // One blocking reader thread per output pipe — the platform's substitute
    // for poll. stderr is merged into stdout by the >[2=1] wrapper, so its
    // pipe EOFs at exec time (no writer left) — it drains pre-exec noise but
    // must not signal child-exit; only stdout EOF means the child is gone.
    spawn_reader(shared.clone(), child.stdout.take().expect("child stdout"), true);
    spawn_reader(shared.clone(), child.stderr.take().expect("child stderr"), false);

    let writer = LineDiscipline {
        stdin: child.stdin.take().expect("child stdin"),
        line: Vec::new(),
        shared: shared.clone(),
        child_pid: child.id(),
    };
    let reader = PtyReader { shared: shared.clone() };

    Ok(Pty { child, reader, writer, shared })
}

impl Pty {
    pub fn child(&self) -> &Child {
        &self.child
    }
}

impl Drop for Pty {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

pub struct PtyReader {
    shared: Arc<Shared>,
}

impl Read for PtyReader {
    fn read(&mut self, target: &mut [u8]) -> io::Result<usize> {
        let mut buf = self.shared.buf.lock().unwrap();
        if buf.is_empty() {
            // Clearing under the buf lock: a racing push (which also holds
            // buf) can't be swallowed.
            if let Some(poller) = self.shared.poller.lock().unwrap().as_ref() {
                poller.shim_clear_ready_read(PTY_READ_WRITE_TOKEN);
            }
            return Err(io::Error::from(io::ErrorKind::WouldBlock));
        }

        let n = target.len().min(buf.len());
        for (i, byte) in buf.drain(..n).enumerate() {
            target[i] = byte;
        }
        if buf.is_empty() {
            if let Some(poller) = self.shared.poller.lock().unwrap().as_ref() {
                poller.shim_clear_ready_read(PTY_READ_WRITE_TOKEN);
            }
        }
        Ok(n)
    }
}

impl EventedReadWrite for Pty {
    type Reader = PtyReader;
    type Writer = LineDiscipline;

    unsafe fn register(
        &mut self,
        poll: &Arc<Poller>,
        mut interest: Event,
        _mode: PollMode,
    ) -> io::Result<()> {
        *self.shared.poller.lock().unwrap() = Some(poll.clone());
        interest.key = PTY_READ_WRITE_TOKEN;
        poll.shim_set_interest(PTY_READ_WRITE_TOKEN, interest);
        poll.shim_set_interest(PTY_CHILD_EVENT_TOKEN, Event::readable(PTY_CHILD_EVENT_TOKEN));

        // Data (or EOF) may have arrived before registration.
        if !self.shared.buf.lock().unwrap().is_empty() {
            poll.shim_set_ready_read(PTY_READ_WRITE_TOKEN);
        }
        if self.shared.eof.load(Ordering::Acquire) {
            poll.shim_set_ready_read(PTY_CHILD_EVENT_TOKEN);
        }
        Ok(())
    }

    fn reregister(&mut self, poll: &Arc<Poller>, mut interest: Event, _: PollMode) -> io::Result<()> {
        interest.key = PTY_READ_WRITE_TOKEN;
        poll.shim_set_interest(PTY_READ_WRITE_TOKEN, interest);
        Ok(())
    }

    fn deregister(&mut self, poll: &Arc<Poller>) -> io::Result<()> {
        poll.shim_remove(PTY_READ_WRITE_TOKEN);
        poll.shim_remove(PTY_CHILD_EVENT_TOKEN);
        *self.shared.poller.lock().unwrap() = None;
        Ok(())
    }

    fn reader(&mut self) -> &mut PtyReader {
        &mut self.reader
    }

    // ponytail: blocking writes — a child that stops reading stalls the
    // reactor thread; add a writer thread if that ever bites.
    fn writer(&mut self) -> &mut LineDiscipline {
        &mut self.writer
    }
}

impl EventedPty for Pty {
    fn next_child_event(&mut self) -> Option<ChildEvent> {
        if !self.shared.eof.load(Ordering::Acquire) {
            return None;
        }
        // EOF on the output pipes is the exit signal (no SIGCHLD here).
        // ponytail: try_wait may race a still-exiting child and lose the
        // status — harmless, Plan 9 exit statuses are strings anyway.
        let status: Option<ExitStatus> = self.child.try_wait().ok().flatten();
        Some(ChildEvent::Exited(status))
    }
}

impl OnResize for Pty {
    fn on_resize(&mut self, window_size: WindowSize) {
        // No TIOCSWINSZ on Plan 9; the live size channel is /env (shared
        // env group — see publish_size). TUIs poll it, shells don't care.
        publish_size(window_size);
    }
}
