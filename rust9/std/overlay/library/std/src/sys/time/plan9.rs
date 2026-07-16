//! Plan 9 time via cc9's clock_gettime (backed by /dev/bintime).
use crate::sync::atomic::{AtomicU64, Ordering};
use crate::time::Duration;

unsafe extern "C" {
    fn clock_gettime(clk: i32, ts: *mut CTimespec) -> i32;
}

/// cc9's `clock_gettime` ignores the clock id and always reads /dev/bintime
/// (realtime ns since epoch) — so CLOCK_MONOTONIC is NOT monotonic: aux/timesync,
/// `date -n`, NTP, or a VM pause can step it backward, which makes `Instant::
/// elapsed()` return 0 and hangs timeout/backoff loops. There is no monotonic
/// kernel clock exposed to us, so we monotonize in userspace the way std did for
/// every non-monotonic platform before vDSO clocks: clamp each reading to the max
/// seen so far, process-wide. Cost is one relaxed CAS loop; the tiny skew a
/// concurrent stepped-back reading could see is bounded by the clamp and never
/// goes negative.
static MONO_MAX_NS: AtomicU64 = AtomicU64::new(0);

fn monotonic_ns() -> u64 {
    let raw = {
        let mut ts = CTimespec { tv_sec: 0, tv_nsec: 0 };
        unsafe { clock_gettime(CLOCK_MONOTONIC, &mut ts) };
        (ts.tv_sec as u64).wrapping_mul(1_000_000_000).wrapping_add(ts.tv_nsec as u64)
    };
    let mut prev = MONO_MAX_NS.load(Ordering::Relaxed);
    loop {
        if raw <= prev {
            return prev; // clock stepped back (or equal): hold the last max
        }
        match MONO_MAX_NS.compare_exchange_weak(prev, raw, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => return raw,
            Err(now) => prev = now,
        }
    }
}

#[repr(C)]
struct CTimespec {
    tv_sec: i64,  // cc9 time_t == long == i64
    tv_nsec: i64, // cc9 long == i64
}

const CLOCK_REALTIME: i32 = 0;
const CLOCK_MONOTONIC: i32 = 1;

fn now(clk: i32) -> Duration {
    let mut ts = CTimespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { clock_gettime(clk, &mut ts) };
    Duration::new(ts.tv_sec as u64, ts.tv_nsec as u32)
}

#[derive(Copy, Clone, PartialEq, Eq, PartialOrd, Ord, Debug, Hash)]
pub struct Instant(Duration);

#[derive(Copy, Clone, PartialEq, Eq, PartialOrd, Ord, Debug, Hash)]
pub struct SystemTime(Duration);

pub const UNIX_EPOCH: SystemTime = SystemTime(Duration::from_secs(0));

impl Instant {
    pub fn now() -> Instant {
        let ns = monotonic_ns();
        Instant(Duration::new(ns / 1_000_000_000, (ns % 1_000_000_000) as u32))
    }

    pub fn checked_sub_instant(&self, other: &Instant) -> Option<Duration> {
        self.0.checked_sub(other.0)
    }

    pub fn checked_add_duration(&self, other: &Duration) -> Option<Instant> {
        Some(Instant(self.0.checked_add(*other)?))
    }

    pub fn checked_sub_duration(&self, other: &Duration) -> Option<Instant> {
        Some(Instant(self.0.checked_sub(*other)?))
    }
}

impl SystemTime {
    pub const MAX: SystemTime = SystemTime(Duration::MAX);
    pub const MIN: SystemTime = SystemTime(Duration::ZERO);

    pub fn now() -> SystemTime {
        SystemTime(now(CLOCK_REALTIME))
    }

    pub fn sub_time(&self, other: &SystemTime) -> Result<Duration, Duration> {
        self.0.checked_sub(other.0).ok_or_else(|| other.0 - self.0)
    }

    pub fn checked_add_duration(&self, other: &Duration) -> Option<SystemTime> {
        Some(SystemTime(self.0.checked_add(*other)?))
    }

    pub fn checked_sub_duration(&self, other: &Duration) -> Option<SystemTime> {
        Some(SystemTime(self.0.checked_sub(*other)?))
    }
}
