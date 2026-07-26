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
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
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
    /// Raw mode: the OR of the independent signals below. There is no
    /// pty/termios to ask for raw mode on Plan 9, so a child's announcements
    /// are the request; while set, the line discipline is bypassed entirely
    /// (no echo, no line buffer, no ICRNL/ONLCR, escape sequences and ^C pass
    /// through).
    raw: AtomicBool,
    /// The signals, tracked SEPARATELY. They are independent: nvim is on the
    /// alternate screen AND uses bracketed paste, and it turns paste off around
    /// its own operations. Treating any single "off" as "leave raw mode" put a
    /// full-screen app back on the cooked line discipline mid-session, which
    /// echoed its keystrokes into the grid and swallowed its escape sequences —
    /// duplicated lines with their tails missing.
    alt_screen: AtomicBool,
    bracketed_paste: AtomicBool,
    /// Kitty keyboard is a STACK: push enters, pop leaves, and apps nest them.
    kitty_depth: AtomicUsize,
}

impl Shared {
    /// Recompute raw from the signals. Raw ends only when every one is off.
    fn refresh_raw(&self) {
        let raw = self.alt_screen.load(Ordering::Acquire)
            || self.bracketed_paste.load(Ordering::Acquire)
            || self.kitty_depth.load(Ordering::Acquire) > 0;
        self.raw.store(raw, Ordering::Release);
        if std::env::var_os("ALACRITTY9_RAWDEBUG").is_some() {
            eprintln!(
                "[a9] raw={raw} (alt={} paste={} kitty={})",
                self.alt_screen.load(Ordering::Acquire),
                self.bracketed_paste.load(Ordering::Acquire),
                self.kitty_depth.load(Ordering::Acquire),
            );
        }
    }

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

/// Sequences that toggle raw mode, matched with a rolling tail so one split
/// across two reads is still seen.
///
/// The alternate screen is the classic signal (vi, nvim, less), but it is not
/// the only one: an app can drive the keyboard while rendering inline, and pi
/// does exactly that. Bracketed paste and the Kitty keyboard push are just as
/// conclusive — a program only asks for either when it is reading keys itself
/// — and without them such an app gets whole cooked LINES, so its Enter never
/// arrives as a keypress at all.
const ALT_ON: [&[u8]; 2] = [b"\x1b[?1049h", b"\x1b[?47h"];
const ALT_OFF: [&[u8]; 2] = [b"\x1b[?1049l", b"\x1b[?47l"];
const PASTE_ON: &[u8] = b"\x1b[?2004h";
const PASTE_OFF: &[u8] = b"\x1b[?2004l";
/// Kitty keyboard protocol: push (CSI > flags u), pop (CSI < u). The flags
/// vary, so the push is matched by shape rather than literally.
const KITTY_POP: &[u8] = b"\x1b[<u";

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
                        // ALACRITTY9_RAWLOG=<path>: every byte the child wrote, for
                        // replaying a session through another emulator.
                        if let Some(path) = std::env::var_os("ALACRITTY9_RAWLOG") {
                            use std::io::Write as _;
                            if let Ok(mut f) =
                                std::fs::OpenOptions::new().create(true).append(true).open(path)
                            {
                                let _ = f.write_all(&chunk[..n]);
                            }
                        }
                        let mut buf = shared.buf.lock().unwrap();
                        // Optionally strip the Kitty keyboard push from the child's
                        // output before the parser sees it (see ALACRITTY9_NOKITTY).
                        let nokitty = std::env::var_os("ALACRITTY9_NOKITTY").is_some();
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
                            // Kitty keyboard push: ESC [ > <digits> u
                            // ALACRITTY9_NOKITTY=1: swallow the child's Kitty keyboard
                            // push so the terminal never enables that encoding and the
                            // app falls back to legacy keys. Diagnostic switch.
                            let kitty_push = byte == b'u' && {
                                let t: Vec<u8> = tail.iter().copied().collect();
                                match t.iter().rposition(|&b| b == b'\x1b') {
                                    Some(esc) if t.len() - esc >= 4 => {
                                        t[esc + 1] == b'['
                                            && t[esc + 2] == b'>'
                                            && t[esc + 3..t.len() - 1]
                                                .iter()
                                                .all(|b| b.is_ascii_digit() || *b == b';')
                                    },
                                    _ => false,
                                }
                            };
                            // Each signal moves only its own flag; raw is their OR.
                            let mut changed = true;
                            if ALT_ON.iter().any(|p| ends_with(p)) {
                                shared.alt_screen.store(true, Ordering::Release);
                            } else if ALT_OFF.iter().any(|p| ends_with(p)) {
                                shared.alt_screen.store(false, Ordering::Release);
                            } else if ends_with(PASTE_ON) {
                                shared.bracketed_paste.store(true, Ordering::Release);
                            } else if ends_with(PASTE_OFF) {
                                shared.bracketed_paste.store(false, Ordering::Release);
                            } else if kitty_push {
                                shared.kitty_depth.fetch_add(1, Ordering::AcqRel);
                            } else if ends_with(KITTY_POP) {
                                let _ = shared.kitty_depth.fetch_update(
                                    Ordering::AcqRel,
                                    Ordering::Acquire,
                                    |d| Some(d.saturating_sub(1)),
                                );
                            } else {
                                changed = false;
                            }
                            if changed {
                                shared.refresh_raw();
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
                            if nokitty && kitty_push {
                                // Drop the whole "ESC [ > flags u" from the stream so the
                                // parser never enables the encoding: the bytes before this
                                // one are already queued, so pop them back off.
                                let t: Vec<u8> = tail.iter().copied().collect();
                                if let Some(esc) = t.iter().rposition(|&b| b == b'\x1b') {
                                    let seq_len = t.len() - esc; // includes the current byte
                                    for _ in 0..seq_len.saturating_sub(1) {
                                        buf.pop_back();
                                    }
                                }
                                continue;
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
/// Cooked is only half the story: when the child announces that it drives the
/// keyboard itself (alt screen, bracketed paste, or a Kitty keyboard push —
/// see RAW_ON) this is bypassed entirely and every byte passes through, which
/// is what a piped child gets instead of the /dev/consctl switch it cannot
/// reach. ALACRITTY9_RAWDEBUG=1 logs the toggles and the mode of each write.
/// Escape-sequence swallow state for cooked mode (see `write`).
#[derive(Clone, Copy, PartialEq)]
enum EscSwallow {
    None,
    /// Saw ESC, deciding the sequence kind.
    Esc,
    /// Inside CSI/SS3: ends at a final byte 0x40-0x7e.
    Csi,
    /// Inside OSC/DCS/APC/PM/SOS: ends at BEL or ST (ESC \).
    Str,
    /// Saw ESC inside a string sequence (possible ST).
    StrEsc,
}

pub struct LineDiscipline {
    stdin: ChildStdin,
    line: Vec<u8>,
    shared: Arc<Shared>,
    child_pid: u32,
    swallow: EscSwallow,
    /// Escape sequences swallowed while cooked (terminal query REPLIES).
    /// A fullscreen app queries the terminal BEFORE its smcup reaches us, so
    /// its replies arrive while we are still cooked — hold them and deliver
    /// the moment raw mode turns on (else nvim times out: E1568). Dropped at
    /// 4KB: a shell session that never goes raw has no use for them.
    pending: Vec<u8>,
}

impl LineDiscipline {
    /// Echo bytes into the terminal by feeding them to the reader buffer, as
    /// if the child had written them.
    fn echo(&self, bytes: &[u8]) {
        let mut buf = self.shared.buf.lock().unwrap();
        buf.extend(bytes);
        self.shared.wake_read();
    }

    /// Hold a swallowed byte for delivery when raw mode turns on.
    fn hold(&mut self, byte: u8) {
        if self.pending.len() < 4096 {
            self.pending.push(byte);
        }
    }

    fn interrupt(&self) {
        let _ = std::fs::write(format!("/proc/{}/notepg", self.child_pid), "interrupt");
    }
}

impl io::Write for LineDiscipline {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        use std::io::Write as _;

        if std::env::var_os("ALACRITTY9_RAWDEBUG").is_some() {
            eprintln!(
                "[a9] write raw={} len={} first={:?}",
                self.shared.raw.load(Ordering::Acquire),
                data.len(),
                data.first()
            );
        }
        // ALACRITTY9_INLOG=<path>: every byte sent TO the child, for comparing what a
        // TUI receives before and after it changes keyboard modes.
        if let Some(path) = std::env::var_os("ALACRITTY9_INLOG") {
            use std::io::Write as _;
            if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
                let _ = writeln!(f, "{:?}", String::from_utf8_lossy(data));
            }
        }

        // Raw mode (child is on the alternate screen): the child owns the
        // byte stream — keystrokes, escape sequences, mouse reports, ^C all
        // pass through verbatim, no echo, no line buffer.
        if self.shared.raw.load(Ordering::Acquire) {
            self.line.clear();
            self.swallow = EscSwallow::None;
            if !self.pending.is_empty() {
                let held = std::mem::take(&mut self.pending);
                self.stdin.write_all(&held)?;
            }
            self.stdin.write_all(data)?;
            self.stdin.flush()?;
            return Ok(data.len());
        }

        for &byte in data {
            // Swallow whole escape sequences. Alacritty writes terminal
            // QUERY REPLIES (DA1 "ESC[?6c", DECRPM, OSC color reports) and
            // key-release/focus reports into the child's stdin; a cooked
            // shell can never use them, and only dropping the control bytes
            // would leak the printable tail ("[?6c") into the line buffer —
            // rc then reads "\x1b[?6cecho hi" and the command "doesn't run".
            // (Replies a fullscreen app requested but exited without reading
            // land here too, once its rmcup flips us back to cooked.)
            match self.swallow {
                EscSwallow::None if byte == 0x1b => {
                    self.swallow = EscSwallow::Esc;
                    self.hold(byte);
                    continue;
                },
                EscSwallow::None => {},
                EscSwallow::Esc => {
                    self.swallow = match byte {
                        b'[' | b'O' => EscSwallow::Csi,
                        b']' | b'P' | b'X' | b'^' | b'_' => EscSwallow::Str,
                        _ => EscSwallow::None, // two-byte sequence, done
                    };
                    self.hold(byte);
                    continue;
                },
                EscSwallow::Csi => {
                    if (0x40..=0x7e).contains(&byte) {
                        self.swallow = EscSwallow::None;
                    }
                    self.hold(byte);
                    continue;
                },
                EscSwallow::Str => {
                    if byte == 0x07 {
                        self.swallow = EscSwallow::None;
                    } else if byte == 0x1b {
                        self.swallow = EscSwallow::StrEsc;
                    }
                    self.hold(byte);
                    continue;
                },
                EscSwallow::StrEsc => {
                    self.swallow =
                        if byte == b'\\' { EscSwallow::None } else { EscSwallow::Str };
                    self.hold(byte);
                    continue;
                },
            }

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
/// Name of the environment variable holding the live-size file's path.
pub const SIZE_FILE_VAR: &str = "A9_SIZE_FILE";

fn publish_size(window_size: WindowSize) {
    // /env alone is NOT a live channel to the child: fork() on this platform uses
    // RFENVG, so the child gets a COPY of the environment group at exec and never
    // sees a later set_var. (Everything worked until the window was resized, and
    // then a TUI kept laying out for the old width — a streaming redraw repeats
    // each row with a few more words on it.) The file below IS shared: its path
    // goes to the child in the environment, and it is rewritten on every resize.
    if let Some(path) = std::env::var_os(SIZE_FILE_VAR) {
        let _ = std::fs::write(path, format!("{} {}\n", window_size.num_cols, window_size.num_lines));
    }
    // Single-threaded env access isn't a real concern on Plan 9 (env vars
    // are per-group /env files), and both callers run on the same thread.
    unsafe {
        std::env::set_var("LINES", window_size.num_lines.to_string());
        std::env::set_var("COLS", window_size.num_cols.to_string());
    }
}

pub fn new(config: &Options, window_size: WindowSize, _window_id: u64) -> io::Result<Pty> {
    // Point the child at a size file it can re-read (see publish_size). One per
    // terminal process; the child inherits the path through its env copy.
    unsafe {
        std::env::set_var(SIZE_FILE_VAR, format!("/tmp/.a9size.{}", std::process::id()));
    }
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
        alt_screen: AtomicBool::new(false),
        bracketed_paste: AtomicBool::new(false),
        kitty_depth: AtomicUsize::new(0),
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
        swallow: EscSwallow::None,
        pending: Vec::new(),
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
