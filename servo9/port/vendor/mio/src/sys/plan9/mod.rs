//! mio backend for Plan 9 (9front) over cc9's `poll()`.
//!
//! # Why not the other backends
//!
//! Plan 9 has no epoll and no kqueue, so mio's `unix` backend (3982 lines) does
//! not apply. The nearest template is `wasip1` (poll-based) — but read its own
//! header: it admits it "only (barely) works using a single thread", has no
//! `Waker`, and cannot register while another thread polls, because it holds its
//! subscription lock across the poll call. Servo is heavily threaded and tokio
//! registers from arbitrary threads while a worker is parked in `poll`, so that
//! design is not usable here.
//!
//! # Design: poll() + self-pipe + edge emulation
//!
//! The classic portable reactor:
//!
//! * `select()` snapshots the registration table, **drops the lock**, and only
//!   then blocks in `poll(2)`. So `register`/`deregister` never wait on a poll.
//!   The poll set is cached and rebuilt only when something changed, so an
//!   idle wakeup costs no allocation and no O(n) copy.
//! * A self-pipe is registered in every poll. `register`, `reregister`,
//!   `deregister`, `Waker::wake` and `rearm` all write one byte to it, which
//!   breaks the in-flight poll so it re-reads the table. Without this, a
//!   registration made during a blocking poll would not be seen until the poll
//!   happened to return — which for an idle connection is "never".
//! * **Edge emulation.** poll(2) is level-triggered, but tokio registers
//!   READABLE|WRITABLE once and never drops write interest, and a connected
//!   /net socket is almost always POLLOUT-ready — re-reporting it every round
//!   would spin the reactor at 100% CPU. So each registration carries
//!   per-direction "reported" bits: a readiness event is delivered only on a
//!   not-ready -> ready transition, and while a direction is reported it is
//!   masked out of the poll set entirely, so poll blocks instead of returning
//!   immediately. The bits clear — and the parked poll is interrupted — when
//!   the app's I/O returns WouldBlock (`IoSourceState::do_io` -> `rearm`):
//!   exactly the "I've drained it, arm the next edge" signal tokio's readiness
//!   model is built on. This is the same shim mio's real backends get from the
//!   kernel (EPOLLET), done in user space.
//! * **Offloaded connect.** A /net connect is a blocking ctl write with no
//!   EINPROGRESS; inline it stalls a reactor worker for the whole handshake.
//!   `tcp::connect` hands it to a helper thread and returns immediately; see
//!   the `connect_offload` module for the full readiness/error story.
//!
//! Plan 9 idiom note: the native answer to concurrency here is a proc per
//! connection blocking on a read, not a readiness reactor. cc9's `poll()` is
//! itself implemented that way (a reader thread per fd). This backend exists to
//! give tokio the epoll-shaped API it demands; it is emulation, and honestly so.
//!
//! Nothing here is a stub: every function does the real thing or returns a real
//! error.

use std::io;
use std::os::fd::RawFd;
#[cfg(all(debug_assertions, feature = "net"))]
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use crate::{Interest, Token};

// --- cc9 / Plan 9 C surface -------------------------------------------------

#[repr(C)]
#[derive(Debug, Copy, Clone)]
struct pollfd {
    fd: i32,
    events: i16,
    revents: i16,
}

// Must match cc9/runtime/include/poll.h.
const POLLIN: i16 = 0x001;
const POLLOUT: i16 = 0x004;
const POLLERR: i16 = 0x008;
const POLLHUP: i16 = 0x010;
const POLLNVAL: i16 = 0x020;

unsafe extern "C" {
    fn poll(fds: *mut pollfd, nfds: u64, timeout: i32) -> i32;
    fn pipe(fds: *mut i32) -> i32;
    fn read(fd: i32, buf: *mut u8, n: usize) -> isize;
    fn write(fd: i32, buf: *const u8, n: usize) -> isize;
    fn close(fd: i32) -> i32;
}

fn last_error() -> io::Error {
    io::Error::last_os_error()
}

// --- Event ------------------------------------------------------------------

/// A readiness event: which token, and what poll(2) reported.
#[derive(Debug, Clone, Copy)]
pub struct Event {
    pub token: Token,
    pub revents: i16,
}

pub type Events = Vec<Event>;

pub mod event {
    use super::{Event, POLLERR, POLLHUP, POLLIN, POLLNVAL, POLLOUT};
    use crate::Token;
    use std::fmt;

    pub fn token(event: &Event) -> Token {
        event.token
    }

    pub fn is_readable(event: &Event) -> bool {
        // HUP counts as readable: a peer that closed leaves a readable EOF, and
        // callers must be woken to observe it.
        event.revents & (POLLIN | POLLHUP) != 0
    }

    pub fn is_writable(event: &Event) -> bool {
        event.revents & POLLOUT != 0
    }

    pub fn is_error(event: &Event) -> bool {
        event.revents & (POLLERR | POLLNVAL) != 0
    }

    pub fn is_read_closed(event: &Event) -> bool {
        event.revents & POLLHUP != 0
    }

    pub fn is_write_closed(event: &Event) -> bool {
        // poll(2) reports one HUP for the whole connection; /net gives no
        // separate write-side shutdown signal, so this mirrors read_closed
        // rather than inventing a distinction.
        event.revents & POLLHUP != 0
    }

    pub fn is_priority(_: &Event) -> bool {
        // POLLPRI exists in the header but /net has no out-of-band channel.
        false
    }

    pub fn is_aio(_: &Event) -> bool {
        false
    }

    pub fn is_lio(_: &Event) -> bool {
        false
    }

    pub fn debug_details(f: &mut fmt::Formatter<'_>, event: &Event) -> fmt::Result {
        write!(f, "token: {:?}, revents: {:#06x}", event.token, event.revents)
    }
}

// --- Selector ---------------------------------------------------------------

#[derive(Debug, Clone, Copy)]
struct Registration {
    fd: RawFd,
    token: Token,
    interests: Interest,
    /// Unique per (re)registration. Post-poll state updates and event delivery
    /// apply only while the generation still matches, so a reregister or
    /// deregister racing an in-flight poll can at worst cause a duplicate
    /// event on the next round — never a suppressed one.
    generation: u64,
    /// Edge-emulation state: a readiness event for the direction was delivered
    /// and no WouldBlock has been seen since. While set, the direction is
    /// masked out of the poll set (see `Table::rebuild`); cleared by `rearm`
    /// (I/O returned WouldBlock) and by `reregister` (fresh edge, like
    /// EPOLL_CTL_MOD re-arming EPOLLET).
    read_reported: bool,
    write_reported: bool,
}

/// Entry in the emit map: what to report when `pollset[i + 1]` fires.
#[derive(Debug, Clone, Copy)]
struct EmitEntry {
    token: Token,
    generation: u64,
}

/// Everything guarded by the one registration lock. The poll set is cached
/// here and rebuilt only when `dirty` — registrations and reported-bits change
/// far less often than `select` wakes up.
#[derive(Debug)]
struct Table {
    regs: Vec<Registration>,
    next_generation: u64,
    dirty: bool,
    /// Cached poll set; slot 0 is always the wake pipe, and `emit[i]`
    /// describes `pollset[i + 1]`. Arc so `select` can snapshot both in O(1)
    /// and drop the lock before blocking in poll().
    pollset: Arc<Vec<pollfd>>,
    emit: Arc<Vec<EmitEntry>>,
}

impl Table {
    fn rebuild(&mut self, wake_r: RawFd) {
        let mut pollset = Vec::with_capacity(self.regs.len() + 1);
        let mut emit = Vec::with_capacity(self.regs.len());
        pollset.push(pollfd { fd: wake_r, events: POLLIN, revents: 0 });
        for r in &self.regs {
            // A source with an offloaded connect in flight (or failed) must
            // not be polled: pre-connect the fd is still the /net ctl file,
            // so its readiness would be garbage (see connect_offload).
            if connect_offload::is_pending(r.fd) {
                continue;
            }
            let mut ev = 0i16;
            if r.interests.is_readable() && !r.read_reported {
                ev |= POLLIN;
            }
            if r.interests.is_writable() && !r.write_reported {
                ev |= POLLOUT;
            }
            // A fully-reported source leaves the poll set: that is what makes
            // the edge emulation actually block instead of spinning on a
            // level-ready fd. cc9's poll only reports ERR/HUP/NVAL under a
            // requested direction, so a parked fd cannot spin us either — a
            // closed-without-deregister fd's POLLNVAL is reported once and
            // then gated like any other readiness.
            if ev == 0 {
                continue;
            }
            pollset.push(pollfd { fd: r.fd, events: ev, revents: 0 });
            emit.push(EmitEntry { token: r.token, generation: r.generation });
        }
        self.pollset = Arc::new(pollset);
        self.emit = Arc::new(emit);
        self.dirty = false;
    }
}

#[derive(Debug)]
struct SelectorInner {
    /// Registered sources + cached poll set. Never locked across a `poll` call.
    table: Mutex<Table>,
    /// Reusable buffer for the kernel-mutable copy of the poll set (poll(2)
    /// writes revents into it). `select` try-locks it, so the common
    /// one-reactor case never allocates; a concurrent select on a clone just
    /// falls back to a fresh Vec.
    scratch: Mutex<Vec<pollfd>>,
    /// Self-pipe. `wake_r` is polled every round; a byte on `wake_w` breaks the
    /// poll so a concurrent registration is picked up immediately.
    wake_r: RawFd,
    wake_w: RawFd,
}

impl Drop for SelectorInner {
    fn drop(&mut self) {
        unsafe {
            close(self.wake_r);
            close(self.wake_w);
        }
    }
}

impl SelectorInner {
    /// Break any in-flight `poll`. Best effort: if the pipe is already full the
    /// reader is guaranteed to wake anyway, so a failed write is not an error.
    fn interrupt(&self) {
        let b = [0u8; 1];
        unsafe {
            write(self.wake_w, b.as_ptr(), 1);
        }
    }

    fn drain_wake(&self) {
        let mut buf = [0u8; 64];
        // wake_r is O_NONBLOCK (set in Selector::new), so a read on an empty pipe
        // returns -1/EAGAIN instead of blocking. Drain until short or empty. The old
        // code broke only on `n < 64`, so an exactly-64-byte backlog fell through to
        // another read — which, on the then-blocking pipe, hung the reactor (M1).
        loop {
            let n = unsafe { read(self.wake_r, buf.as_mut_ptr(), buf.len()) };
            if n <= 0 || (n as usize) < buf.len() {
                break;
            }
        }
    }

    /// Clear `fd`'s reported-ready bits: its I/O returned WouldBlock, so the
    /// NEXT readiness must be delivered again. The in-flight poll's set was
    /// built WITHOUT this fd's reported directions, so only a rebuilt poll can
    /// observe that readiness — hence the interrupt, exactly like the one that
    /// closes the register-during-poll race. (This adds one wake byte per
    /// WouldBlock; it leans on M1's non-blocking wake pipe the same way
    /// register/deregister already do.)
    fn rearm(&self, fd: RawFd) {
        let mut cleared = false;
        {
            let mut guard = self.table.lock().unwrap();
            let table = &mut *guard;
            if let Some(r) = table.regs.iter_mut().find(|r| r.fd == fd) {
                if r.read_reported || r.write_reported {
                    r.read_reported = false;
                    r.write_reported = false;
                    table.dirty = true;
                    cleared = true;
                }
            }
        }
        if cleared {
            self.interrupt();
        }
    }

    /// Force a poll-set rebuild and break any in-flight poll. Used by the
    /// connect-offload helper when a connect finishes.
    fn refresh(&self) {
        self.table.lock().unwrap().dirty = true;
        self.interrupt();
    }
}

/// Only used to give each Selector a debug-only identity (see `Selector::id`),
/// so it does not exist in release.
#[cfg(all(debug_assertions, feature = "net"))]
static NEXT_ID: AtomicUsize = AtomicUsize::new(1);

#[derive(Debug)]
pub struct Selector {
    #[cfg(all(debug_assertions, feature = "net"))]
    id: usize,
    inner: Arc<SelectorInner>,
}

impl Selector {
    pub fn new() -> io::Result<Selector> {
        let mut fds = [0i32; 2];
        if unsafe { pipe(fds.as_mut_ptr()) } < 0 {
            return Err(last_error());
        }
        // M1: make BOTH ends non-blocking. wake_r so `drain_wake` never blocks once
        // the pipe is empty (the old drain looped into a blocking read on an exactly-
        // full buffer and hung the whole reactor). wake_w so `interrupt`/`rearm`/
        // `Waker::wake` never block writing when the pipe is full — a full pipe already
        // means a wake is pending, so a dropped byte is harmless (see `interrupt`).
        for &fd in fds.iter() {
            let fl = unsafe { libc::fcntl(fd, libc::F_GETFL) };
            if fl < 0 || unsafe { libc::fcntl(fd, libc::F_SETFL, fl | libc::O_NONBLOCK) } < 0 {
                let e = last_error();
                unsafe {
                    close(fds[0]);
                    close(fds[1]);
                }
                return Err(e);
            }
        }
        Ok(Selector {
            #[cfg(all(debug_assertions, feature = "net"))]
            id: NEXT_ID.fetch_add(1, Ordering::Relaxed),
            inner: Arc::new(SelectorInner {
                table: Mutex::new(Table {
                    regs: Vec::new(),
                    next_generation: 0,
                    dirty: true, // first select builds the poll set
                    pollset: Arc::new(Vec::new()),
                    emit: Arc::new(Vec::new()),
                }),
                scratch: Mutex::new(Vec::new()),
                wake_r: fds[0],
                wake_w: fds[1],
            }),
        })
    }

    #[cfg(all(debug_assertions, feature = "net"))]
    pub fn id(&self) -> usize {
        self.id
    }

    pub fn try_clone(&self) -> io::Result<Selector> {
        // Shares the registration table and the wake pipe, so a clone sees the
        // same sources and the same interrupts.
        Ok(Selector {
            #[cfg(all(debug_assertions, feature = "net"))]
            id: self.id,
            inner: self.inner.clone(),
        })
    }

    pub fn select(&self, events: &mut Events, timeout: Option<Duration>) -> io::Result<()> {
        events.clear();

        // Snapshot the cached poll set, then DROP the lock. Holding it across
        // poll() is what makes the wasip1 backend single-threaded-only.
        let (pollset, emit) = {
            let mut guard = self.inner.table.lock().unwrap();
            let table = &mut *guard;

            // Surface finished-but-failed offloaded connects as one
            // writable+error event, through the same reported-bits gate as
            // real events so a dead fd cannot spin the reactor. The errno
            // itself was latched by net9's connect and comes back through the
            // standard take_error -> getsockopt(SO_ERROR) protocol.
            if connect_offload::pending_len() > 0 {
                for ffd in connect_offload::failed_fds() {
                    if let Some(r) = table.regs.iter_mut().find(|r| r.fd == ffd) {
                        if !r.write_reported {
                            events.push(Event { token: r.token, revents: POLLOUT | POLLERR });
                            r.read_reported = true;
                            r.write_reported = true;
                            table.dirty = true;
                        }
                    }
                }
            }

            if table.dirty {
                table.rebuild(self.inner.wake_r);
            }
            (Arc::clone(&table.pollset), Arc::clone(&table.emit))
        };

        // poll(2) writes revents, so it needs a mutable copy of the snapshot;
        // reuse the scratch buffer to keep the steady state allocation-free.
        let mut local = Vec::new();
        let mut scratch = self.inner.scratch.try_lock().ok();
        let fds: &mut Vec<pollfd> = scratch.as_deref_mut().unwrap_or(&mut local);
        fds.clear();
        fds.extend_from_slice(&pollset);

        let timeout_ms = if !events.is_empty() {
            0 // a synthetic event is already pending; just harvest what's ready
        } else {
            match timeout {
                // Saturate rather than wrap: a huge Duration must mean "a long
                // time", never a short one.
                Some(d) => d.as_millis().min(i32::MAX as u128) as i32,
                None => -1,
            }
        };

        let n = unsafe { poll(fds.as_mut_ptr(), fds.len() as u64, timeout_ms) };
        if n < 0 {
            let err = last_error();
            // An interrupted poll is not a failure; report no events and let the
            // caller loop.
            if err.kind() == io::ErrorKind::Interrupted {
                return Ok(());
            }
            return Err(err);
        }
        if n == 0 {
            return Ok(()); // timed out (synthetic events, if any, still delivered)
        }

        // Slot 0: we were interrupted (a registration or reported-bit changed,
        // or Waker fired).
        if fds[0].revents & POLLIN != 0 {
            self.inner.drain_wake();
        }

        {
            let mut guard = self.inner.table.lock().unwrap();
            let table = &mut *guard;
            for (i, e) in emit.iter().enumerate() {
                let revents = fds[i + 1].revents;
                if revents == 0 {
                    continue;
                }
                // Deliver only while this exact registration incarnation is
                // still current. If a reregister/deregister raced the poll,
                // drop the stale event: those paths already interrupted, so
                // the next round re-polls a fresh table and re-reports any
                // readiness that still stands. Setting the reported bits here,
                // on the live entry, makes delivery and suppression one atomic
                // step — a bit is only ever set for an incarnation whose event
                // the caller is actually receiving.
                if let Some(r) = table.regs.iter_mut().find(|r| r.generation == e.generation) {
                    // ERR/HUP/NVAL wake both directions (a read sees EOF/error
                    // immediately, a write fails immediately), so they latch
                    // both gates; re-reported only after a WouldBlock proves
                    // the app is still listening. No spin from a dead fd. An
                    // event is delivered iff it latches at least one NEW
                    // direction — a concurrent select on a clone (which polls
                    // the same set) then cannot deliver the same edge twice.
                    let mut newly = false;
                    if revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL) != 0 && !r.read_reported {
                        r.read_reported = true;
                        newly = true;
                    }
                    if revents & (POLLOUT | POLLHUP | POLLERR | POLLNVAL) != 0 && !r.write_reported
                    {
                        r.write_reported = true;
                        newly = true;
                    }
                    if newly {
                        table.dirty = true;
                        events.push(Event { token: e.token, revents });
                    }
                }
            }
        }
        Ok(())
    }

    pub fn register(&self, fd: RawFd, token: Token, interests: Interest) -> io::Result<()> {
        {
            let mut guard = self.inner.table.lock().unwrap();
            let table = &mut *guard;
            if table.regs.iter().any(|r| r.fd == fd) {
                return Err(io::Error::new(
                    io::ErrorKind::AlreadyExists,
                    "fd already registered",
                ));
            }
            let generation = table.next_generation;
            table.next_generation += 1;
            table.regs.push(Registration {
                fd,
                token,
                interests,
                generation,
                read_reported: false,
                write_reported: false,
            });
            table.dirty = true;
        }
        // Wake any in-flight poll so it picks this up now, not whenever it
        // happens to return.
        self.inner.interrupt();
        Ok(())
    }

    pub fn reregister(&self, fd: RawFd, token: Token, interests: Interest) -> io::Result<()> {
        {
            let mut guard = self.inner.table.lock().unwrap();
            let table = &mut *guard;
            let generation = table.next_generation;
            match table.regs.iter_mut().find(|r| r.fd == fd) {
                Some(r) => {
                    r.token = token;
                    r.interests = interests;
                    // A fresh registration wants a fresh edge: clear the gates
                    // so current readiness is re-reported (EPOLL_CTL_MOD does
                    // the same for EPOLLET), and retire the old generation so
                    // an in-flight poll's stale event is dropped instead of
                    // re-latching the gates it just opened.
                    r.generation = generation;
                    r.read_reported = false;
                    r.write_reported = false;
                }
                None => return Err(io::ErrorKind::NotFound.into()),
            }
            table.next_generation += 1;
            table.dirty = true;
        }
        self.inner.interrupt();
        Ok(())
    }

    pub fn deregister(&self, fd: RawFd) -> io::Result<()> {
        {
            let mut guard = self.inner.table.lock().unwrap();
            let table = &mut *guard;
            match table.regs.iter().position(|r| r.fd == fd) {
                Some(i) => {
                    table.regs.swap_remove(i);
                }
                None => return Err(io::ErrorKind::NotFound.into()),
            }
            table.dirty = true;
        }
        self.inner.interrupt();
        Ok(())
    }

    fn wake_handle(&self) -> Arc<SelectorInner> {
        self.inner.clone()
    }
}

// --- Waker ------------------------------------------------------------------

/// Wakes a `Poll` from another thread by writing to the selector's self-pipe.
#[derive(Debug)]
pub struct Waker {
    inner: Arc<SelectorInner>,
}

impl Waker {
    pub fn new(selector: &Selector, _token: Token) -> io::Result<Waker> {
        // The token is unused: an interrupt is not a source event. `select`
        // drains the pipe and reports nothing for it, which is what mio expects
        // of a Waker on backends where waking is not itself a readiness event.
        Ok(Waker { inner: selector.wake_handle() })
    }

    pub fn wake(&self) -> io::Result<()> {
        self.inner.interrupt();
        Ok(())
    }
}

// --- offloaded connect --------------------------------------------------------

/// A /net connect is a blocking write to the conn's ctl file: it returns when
/// the handshake is done, and there is no EINPROGRESS. Done inline it parks a
/// reactor worker for a full RTT+, freezing every task on that worker. So
/// `tcp::connect` hands the blocking call to a short-lived helper thread and
/// returns immediately, as if the kernel had said EINPROGRESS; this module is
/// the bookkeeping that makes the readiness story come out right:
///
/// * While a connect is in flight (or has failed) the fd stays OUT of the poll
///   set — pre-connect the fd is still the /net ctl file, so polling it would
///   report garbage readiness (and cc9's poll would park a reader thread on
///   the ctl file).
/// * On success the helper removes the entry and refreshes the registered
///   selector: the now-connected socket re-enters the poll set and reports
///   POLLOUT through the normal edge gate — exactly what tokio's connect
///   future is waiting on.
/// * On failure `select` synthesizes one POLLOUT|POLLERR event (gated by the
///   same reported bits, so it cannot spin). The errno itself is already
///   latched by net9's connect for getsockopt(SO_ERROR), so the standard
///   poll-writable-then-take_error protocol returns the real error unchanged.
///
/// Lock order: the selector table lock may be held while taking PENDING
/// (select and rebuild do), so this module NEVER takes the table lock while
/// holding PENDING.
mod connect_offload {
    use std::os::fd::RawFd;
    use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
    use std::sync::{Arc, Mutex};

    use super::SelectorInner;

    pub(super) enum Pending {
        /// Helper still blocked in connect(). The selector is filled in when
        /// the fd gets registered, so completion can break its poll.
        InFlight { selector: Option<Arc<SelectorInner>> },
        /// Connect failed; `select` synthesizes the error event. Removed on
        /// deregister.
        Failed,
    }

    // ponytail: one thread per in-flight connect, capped. Above the cap the
    // extras just degrade to today's synchronous connect; a real pool can
    // replace this if that ever shows up in a profile.
    //
    // DISABLED (cap = 0 => begin() always returns None => every connect is
    // synchronous, exactly the pre-offload behavior). Two review findings block the
    // offload: (1) a fd closed while its helper is still parked in cc9 connect(fd)
    // is a use-after-free/wrong-socket at the net9 C layer (ns_tab has no locking) —
    // reachable from `tokio::time::timeout(d, TcpStream::connect(..))`; (2) a Failed
    // or still-InFlight entry whose fd is closed without deregister lingers in this
    // process-global table forever and poisons the reused fd number (permanent
    // hang). Both are fixable only with net9-level connect safety (helper owns a
    // dup, or a connect-in-progress refcount); until then the latency win isn't
    // worth a UAF. The table/ticket machinery below stays inert + forward-compatible
    // — flip this back to a positive cap once net9 connect is made close-safe.
    const MAX_IN_FLIGHT: usize = 0;

    /// fd -> state, keyed with a unique ticket so a stale helper finishing
    /// after the fd was deregistered (and possibly reused) can never touch the
    /// new owner's entry.
    static PENDING: Mutex<Vec<(RawFd, u64, Pending)>> = Mutex::new(Vec::new());
    /// Entry-count mirror so the hot paths (every select/rebuild) skip the
    /// lock entirely when nothing is pending — i.e. almost always.
    static PENDING_LEN: AtomicUsize = AtomicUsize::new(0);
    static NEXT_TICKET: AtomicU64 = AtomicU64::new(0);

    pub(super) fn pending_len() -> usize {
        PENDING_LEN.load(Ordering::Acquire)
    }

    pub(super) fn is_pending(fd: RawFd) -> bool {
        pending_len() > 0 && PENDING.lock().unwrap().iter().any(|&(f, _, _)| f == fd)
    }

    pub(super) fn failed_fds() -> Vec<RawFd> {
        PENDING
            .lock()
            .unwrap()
            .iter()
            .filter(|(_, _, s)| matches!(s, Pending::Failed))
            .map(|&(fd, _, _)| fd)
            .collect()
    }

    /// Claim an offload slot. `None` = at capacity; the caller connects inline
    /// (today's behavior).
    pub(super) fn begin(fd: RawFd) -> Option<u64> {
        let mut p = PENDING.lock().unwrap();
        let in_flight = p
            .iter()
            .filter(|(_, _, s)| matches!(s, Pending::InFlight { .. }))
            .count();
        if in_flight >= MAX_IN_FLIGHT {
            return None;
        }
        let ticket = NEXT_TICKET.fetch_add(1, Ordering::Relaxed);
        p.push((fd, ticket, Pending::InFlight { selector: None }));
        PENDING_LEN.store(p.len(), Ordering::Release);
        Some(ticket)
    }

    /// Thread spawn failed: retract the entry; the caller falls back to the
    /// inline connect.
    pub(super) fn cancel(fd: RawFd, ticket: u64) {
        let mut p = PENDING.lock().unwrap();
        p.retain(|&(f, t, _)| !(f == fd && t == ticket));
        PENDING_LEN.store(p.len(), Ordering::Release);
    }

    /// Helper-thread completion. On success the entry just disappears and the
    /// now-connected fd re-enters the poll set; on failure it flips to Failed
    /// for `select` to report. Either way the registered selector (if any) is
    /// refreshed so a parked poll notices immediately.
    pub(super) fn finish(fd: RawFd, ticket: u64, ok: bool) {
        let selector = {
            let mut p = PENDING.lock().unwrap();
            let Some(i) = p.iter().position(|&(f, t, _)| f == fd && t == ticket) else {
                return; // deregistered while in flight; nobody left to tell
            };
            let selector = match &mut p[i].2 {
                Pending::InFlight { selector } => selector.take(),
                Pending::Failed => None,
            };
            if ok {
                p.remove(i);
            } else {
                p[i].2 = Pending::Failed;
            }
            PENDING_LEN.store(p.len(), Ordering::Release);
            selector
        }; // PENDING dropped before touching the table lock (lock order)
        if let Some(s) = selector {
            s.refresh();
        }
    }

    /// Registration-time hookup: remember the selector so `finish` can break
    /// its poll. A no-op unless `fd` has a connect in flight.
    pub(super) fn attach(fd: RawFd, sel: &Arc<SelectorInner>) {
        if pending_len() == 0 {
            return;
        }
        let mut p = PENDING.lock().unwrap();
        for e in p.iter_mut() {
            if e.0 == fd {
                if let Pending::InFlight { selector } = &mut e.2 {
                    *selector = Some(Arc::clone(sel));
                }
            }
        }
    }

    /// Deregistration cleanup: drop any state for `fd` so a later reuse of the
    /// fd number starts clean. An in-flight helper finishing afterwards finds
    /// its ticket gone and does nothing.
    pub(super) fn forget(fd: RawFd) {
        if pending_len() == 0 {
            return;
        }
        let mut p = PENDING.lock().unwrap();
        p.retain(|&(f, _, _)| f != fd);
        PENDING_LEN.store(p.len(), Ordering::Release);
    }
}

// --- IoSourceState ----------------------------------------------------------

cfg_io_source! {
    use crate::Registry;

    // Unlike epoll/kqueue the edge emulation lives in user space, so each
    // source keeps a handle back to its selector: `do_io` must clear the
    // reported-ready bits when an op returns WouldBlock — the "drained, arm
    // the next edge" signal (see `SelectorInner::rearm`). Mirrors how
    // sys/unix/selector/poll.rs rearms through its own IoSourceState.
    pub(crate) struct IoSourceState {
        inner: Option<(Arc<SelectorInner>, RawFd)>,
    }

    impl IoSourceState {
        pub(crate) fn new() -> IoSourceState {
            IoSourceState { inner: None }
        }

        pub(crate) fn do_io<T, F, R>(&self, f: F, io: &T) -> io::Result<R>
        where
            F: FnOnce(&T) -> io::Result<R>,
        {
            let result = f(io);
            if let Err(err) = &result {
                if err.kind() == io::ErrorKind::WouldBlock {
                    if let Some((selector, fd)) = &self.inner {
                        selector.rearm(*fd);
                    }
                }
            }
            result
        }

        pub(crate) fn register(
            &mut self,
            registry: &Registry,
            token: Token,
            interests: Interest,
            fd: RawFd,
        ) -> io::Result<()> {
            let selector = registry.selector();
            let handle = selector.wake_handle();
            // Late-bind an offloaded connect (started before registration, as
            // tokio always does) to this selector so its completion can break
            // our poll. This MUST happen before the table insert: attach-first
            // means a connect finishing at any point either finds the handle
            // (and refreshes us), or finished before it — in which case the fd
            // is no longer pending and register's own dirty+interrupt below
            // puts it straight into the poll set. Attach-after would leave a
            // window where a rebuild skipped the in-flight fd and the
            // handle-less completion could never put it back: a lost wakeup.
            connect_offload::attach(fd, &handle);
            selector.register(fd, token, interests)?;
            self.inner = Some((handle, fd));
            Ok(())
        }

        pub(crate) fn reregister(
            &mut self,
            registry: &Registry,
            token: Token,
            interests: Interest,
            fd: RawFd,
        ) -> io::Result<()> {
            registry.selector().reregister(fd, token, interests)
        }

        pub(crate) fn deregister(&mut self, registry: &Registry, fd: RawFd) -> io::Result<()> {
            let result = registry.selector().deregister(fd);
            connect_offload::forget(fd);
            self.inner = None;
            result
        }
    }
}

// --- net --------------------------------------------------------------------

cfg_net! {
    use std::mem::{size_of, MaybeUninit};
    use std::net::{self, SocketAddr};
    use std::os::fd::{AsRawFd, FromRawFd};

    /// SocketAddr -> a cc9 `sockaddr` + length. Layouts come from
    /// cc9/runtime/include/netinet/in.h via the plan9 libc module.
    fn socket_addr(addr: &SocketAddr) -> (libc::sockaddr_storage, libc::socklen_t) {
        let mut storage: libc::sockaddr_storage = unsafe { std::mem::zeroed() };
        match addr {
            SocketAddr::V4(a) => {
                let sin = libc::sockaddr_in {
                    sin_family: libc::AF_INET as libc::sa_family_t,
                    sin_port: a.port().to_be(),
                    sin_addr: libc::in_addr { s_addr: u32::from_ne_bytes(a.ip().octets()) },
                    sin_zero: [0; 8],
                };
                unsafe {
                    std::ptr::copy_nonoverlapping(
                        &sin as *const _ as *const u8,
                        &mut storage as *mut _ as *mut u8,
                        size_of::<libc::sockaddr_in>(),
                    );
                }
                (storage, size_of::<libc::sockaddr_in>() as libc::socklen_t)
            }
            SocketAddr::V6(a) => {
                let sin6 = libc::sockaddr_in6 {
                    sin6_family: libc::AF_INET6 as libc::sa_family_t,
                    sin6_port: a.port().to_be(),
                    sin6_flowinfo: a.flowinfo(),
                    sin6_addr: libc::in6_addr { s6_addr: a.ip().octets() },
                    sin6_scope_id: a.scope_id(),
                };
                unsafe {
                    std::ptr::copy_nonoverlapping(
                        &sin6 as *const _ as *const u8,
                        &mut storage as *mut _ as *mut u8,
                        size_of::<libc::sockaddr_in6>(),
                    );
                }
                (storage, size_of::<libc::sockaddr_in6>() as libc::socklen_t)
            }
        }
    }

    fn new_socket(domain: i32, ty: i32) -> io::Result<i32> {
        let fd = unsafe { libc::socket(domain, ty, 0) };
        if fd < 0 {
            return Err(last_error());
        }
        // mio requires non-blocking sources; cc9 honours O_NONBLOCK via fcntl.
        let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
        if flags < 0 || unsafe { libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) } < 0 {
            let e = last_error();
            unsafe { libc::close(fd) };
            return Err(e);
        }
        Ok(fd)
    }

    pub(crate) mod tcp {
        use super::*;

        pub(crate) fn new_for_addr(address: SocketAddr) -> io::Result<i32> {
            let domain = match address {
                SocketAddr::V4(_) => libc::AF_INET,
                SocketAddr::V6(_) => libc::AF_INET6,
            };
            new_socket(domain, libc::SOCK_STREAM)
        }

        pub(crate) fn bind(socket: &net::TcpListener, addr: SocketAddr) -> io::Result<()> {
            let (raw, len) = socket_addr(&addr);
            let r = unsafe {
                libc::bind(socket.as_raw_fd(), &raw as *const _ as *const libc::sockaddr, len)
            };
            if r < 0 { Err(last_error()) } else { Ok(()) }
        }

        /// NB: cc9's connect() is SYNCHRONOUS even on an O_NONBLOCK socket — a
        /// /net connect is a write to the conn's ctl file, and there is no
        /// EINPROGRESS. Inline that stalls the calling reactor thread for the
        /// whole handshake, so the blocking call is offloaded to a helper
        /// thread (see connect_offload) and this returns immediately as if the
        /// kernel had said EINPROGRESS. At capacity, or if the thread cannot
        /// spawn, it degrades to the old synchronous behavior. The inline path
        /// accepts EINPROGRESS anyway in case net9 ever grows it.
        pub(crate) fn connect(socket: &net::TcpStream, addr: SocketAddr) -> io::Result<()> {
            let fd = socket.as_raw_fd();
            let (raw, len) = socket_addr(&addr);

            if let Some(ticket) = connect_offload::begin(fd) {
                let spawned = std::thread::Builder::new()
                    .name("mio-connect".to_string())
                    .spawn(move || {
                        let r = unsafe {
                            libc::connect(fd, &raw as *const _ as *const libc::sockaddr, len)
                        };
                        // On failure net9 has latched errno for SO_ERROR; all
                        // the reactor needs from us is the wakeup.
                        connect_offload::finish(fd, ticket, r >= 0);
                    });
                match spawned {
                    Ok(_) => return Ok(()),
                    Err(_) => connect_offload::cancel(fd, ticket),
                }
            }

            let r = unsafe {
                libc::connect(fd, &raw as *const _ as *const libc::sockaddr, len)
            };
            if r < 0 {
                let err = last_error();
                // EINPROGRESS (115 on linux) is the async-connect signal.
                if err.raw_os_error() != Some(115) {
                    return Err(err);
                }
            }
            Ok(())
        }

        pub(crate) fn listen(socket: &net::TcpListener, backlog: i32) -> io::Result<()> {
            let r = unsafe { libc::listen(socket.as_raw_fd(), backlog) };
            if r < 0 { Err(last_error()) } else { Ok(()) }
        }

        pub(crate) fn set_reuseaddr(socket: &net::TcpListener, reuseaddr: bool) -> io::Result<()> {
            let val: i32 = i32::from(reuseaddr);
            let r = unsafe {
                libc::setsockopt(
                    socket.as_raw_fd(),
                    libc::SOL_SOCKET,
                    libc::SO_REUSEADDR,
                    &val as *const i32 as *const std::ffi::c_void,
                    size_of::<i32>() as libc::socklen_t,
                )
            };
            if r < 0 { Err(last_error()) } else { Ok(()) }
        }

        pub(crate) fn accept(
            listener: &net::TcpListener,
        ) -> io::Result<(net::TcpStream, SocketAddr)> {
            // No accept4(2) on Plan 9; accept then set non-blocking.
            let mut addr: MaybeUninit<libc::sockaddr_storage> = MaybeUninit::uninit();
            let mut length = size_of::<libc::sockaddr_storage>() as libc::socklen_t;
            let fd = unsafe {
                libc::accept(
                    listener.as_raw_fd(),
                    addr.as_mut_ptr() as *mut libc::sockaddr,
                    &mut length,
                )
            };
            if fd < 0 {
                return Err(last_error());
            }
            let stream = unsafe { net::TcpStream::from_raw_fd(fd) };
            stream.set_nonblocking(true)?;
            let peer = stream.peer_addr()?;
            Ok((stream, peer))
        }
    }

    pub(crate) mod udp {
        use super::*;

        pub(crate) fn bind(addr: SocketAddr) -> io::Result<net::UdpSocket> {
            let socket = net::UdpSocket::bind(addr)?;
            socket.set_nonblocking(true)?;
            Ok(socket)
        }

        pub(crate) fn only_v6(_: &net::UdpSocket) -> io::Result<bool> {
            // /net exposes no IPV6_V6ONLY state to query.
            Err(io::ErrorKind::Unsupported.into())
        }
    }
}
