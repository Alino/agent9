//! Plan 9 stand-in for the `polling` crate (API subset alacritty_terminal uses).
//!
//! 9front has no poll/select/epoll; the platform idiom is a blocking reader
//! thread per fd. Readiness here is *pushed* by those threads via the shim_*
//! methods (used by alacritty_terminal's tty/plan9.rs), and `wait` is a
//! condvar sleep. Readiness is sticky per key until explicitly cleared, which
//! reproduces level-triggered epoll semantics: the event loop keeps waking
//! until the owner of the buffer drains it and clears the flag.

use std::collections::HashMap;
use std::io;
use std::num::NonZeroUsize;
use std::sync::{Condvar, Mutex};
use std::time::Duration;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PollMode {
    Oneshot,
    Level,
    Edge,
}

#[derive(Debug, Clone, Copy, Default)]
pub struct Event {
    pub key: usize,
    pub readable: bool,
    pub writable: bool,
}

impl Event {
    pub fn readable(key: usize) -> Self {
        Event { key, readable: true, writable: false }
    }

    pub fn writable(key: usize) -> Self {
        Event { key, readable: false, writable: true }
    }

    pub fn all(key: usize) -> Self {
        Event { key, readable: true, writable: true }
    }

    pub fn none(key: usize) -> Self {
        Event { key, readable: false, writable: false }
    }

    pub fn is_interrupt(&self) -> bool {
        false
    }
}

pub struct Events {
    list: Vec<Event>,
}

impl Events {
    pub fn new() -> Self {
        Events { list: Vec::new() }
    }

    pub fn with_capacity(cap: NonZeroUsize) -> Self {
        Events { list: Vec::with_capacity(cap.get()) }
    }

    pub fn clear(&mut self) {
        self.list.clear();
    }

    pub fn is_empty(&self) -> bool {
        self.list.is_empty()
    }

    pub fn len(&self) -> usize {
        self.list.len()
    }

    pub fn iter(&self) -> impl Iterator<Item = Event> + '_ {
        self.list.iter().copied()
    }
}

impl Default for Events {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Default)]
struct Inner {
    /// key -> (interest, sticky readable readiness).
    sources: HashMap<usize, (Event, bool)>,
    notified: bool,
}

impl Inner {
    /// Events reportable right now: sticky-readable sources with read
    /// interest, plus write-interested sources (a blocking pipe write is
    /// always "ready" — it just blocks; matches level-triggered epoll on a
    /// writable pipe).
    fn collect(&self, out: &mut Events) -> usize {
        let mut n = 0;
        for (&key, &(interest, ready_read)) in &self.sources {
            let ev = Event {
                key,
                readable: interest.readable && ready_read,
                writable: interest.writable,
            };
            if ev.readable || ev.writable {
                out.list.push(ev);
                n += 1;
            }
        }
        n
    }
}

pub struct Poller {
    inner: Mutex<Inner>,
    cond: Condvar,
}

impl Poller {
    pub fn new() -> io::Result<Self> {
        Ok(Poller { inner: Mutex::new(Inner::default()), cond: Condvar::new() })
    }

    /// Wake `wait` without reporting any event (the caller re-checks its
    /// channel, exactly like polling's notify).
    pub fn notify(&self) -> io::Result<()> {
        let mut inner = self.inner.lock().unwrap();
        inner.notified = true;
        self.cond.notify_all();
        Ok(())
    }

    pub fn wait(&self, events: &mut Events, timeout: Option<Duration>) -> io::Result<usize> {
        let deadline = timeout.map(|t| std::time::Instant::now() + t);
        let mut inner = self.inner.lock().unwrap();

        loop {
            if inner.notified {
                inner.notified = false;
                return Ok(inner.collect(events));
            }

            let n = inner.collect(events);
            if n > 0 {
                return Ok(n);
            }

            match deadline {
                None => inner = self.cond.wait(inner).unwrap(),
                Some(d) => {
                    let now = std::time::Instant::now();
                    if now >= d {
                        return Ok(0);
                    }
                    let (guard, _) = self.cond.wait_timeout(inner, d - now).unwrap();
                    inner = guard;
                },
            }
        }
    }

    // ---- shim extensions (not in the real polling API) ----
    // Used by alacritty_terminal's tty/plan9.rs reader/child threads.

    pub fn shim_set_interest(&self, key: usize, interest: Event) {
        let mut inner = self.inner.lock().unwrap();
        let entry = inner.sources.entry(key).or_insert((interest, false));
        entry.0 = interest;
        self.cond.notify_all();
    }

    pub fn shim_remove(&self, key: usize) {
        self.inner.lock().unwrap().sources.remove(&key);
    }

    /// Mark a source readable (sticky) and wake the poller.
    pub fn shim_set_ready_read(&self, key: usize) {
        let mut inner = self.inner.lock().unwrap();
        inner.sources.entry(key).or_insert((Event::none(key), false)).1 = true;
        self.cond.notify_all();
    }

    /// Clear sticky readability — called by the buffer owner once drained.
    pub fn shim_clear_ready_read(&self, key: usize) {
        if let Some(entry) = self.inner.lock().unwrap().sources.get_mut(&key) {
            entry.1 = false;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use std::time::Instant;

    #[test]
    fn level_trigger_and_notify() {
        let p = Arc::new(Poller::new().unwrap());
        let mut evs = Events::new();

        // Timeout path: no sources, short timeout.
        let t = Instant::now();
        assert_eq!(p.wait(&mut evs, Some(Duration::from_millis(10))).unwrap(), 0);
        assert!(t.elapsed() >= Duration::from_millis(10));

        // Sticky readable stays reported until cleared (level-triggered).
        p.shim_set_interest(0, Event::readable(0));
        p.shim_set_ready_read(0);
        evs.clear();
        assert_eq!(p.wait(&mut evs, None).unwrap(), 1);
        evs.clear();
        assert_eq!(p.wait(&mut evs, None).unwrap(), 1);
        p.shim_clear_ready_read(0);
        evs.clear();
        assert_eq!(p.wait(&mut evs, Some(Duration::from_millis(5))).unwrap(), 0);

        // Write interest is always ready; notify wakes with no events.
        p.shim_set_interest(0, Event { key: 0, readable: true, writable: true });
        evs.clear();
        assert_eq!(p.wait(&mut evs, None).unwrap(), 1);
        assert!(evs.iter().next().unwrap().writable);
        p.shim_set_interest(0, Event::none(0));
        p.notify().unwrap();
        evs.clear();
        assert_eq!(p.wait(&mut evs, None).unwrap(), 0);

        // Cross-thread wake.
        let p2 = p.clone();
        let h = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(20));
            p2.shim_set_ready_read(7);
        });
        p.shim_set_interest(7, Event::readable(7));
        evs.clear();
        assert_eq!(p.wait(&mut evs, None).unwrap(), 1);
        assert_eq!(evs.iter().next().unwrap().key, 7);
        h.join().unwrap();
    }
}
