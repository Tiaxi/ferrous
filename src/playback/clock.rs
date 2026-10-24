// SPDX-License-Identifier: GPL-3.0-or-later

//! Time stamps for playback positions.
//!
//! The engine reports the position it read from the pipeline together with
//! the time it read it, on `CLOCK_MONOTONIC`. The UI reads the same clock
//! (`ui/src/PlaybackClock.cpp`), so it can tell how old a position is when
//! it arrives and extrapolate it without a second estimate of its own.
//! `CLOCK_MONOTONIC` does not jump when the wall clock is set, and both
//! processes share it, unlike `Instant`, whose readings cannot cross the
//! FFI boundary.

use std::sync::Arc;
use std::time::Duration;

/// A reading of `CLOCK_MONOTONIC`, in nanoseconds since an unspecified
/// start that every process on the machine shares.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct MonotonicTime(u64);

impl MonotonicTime {
    #[must_use]
    pub const fn from_nanos(nanos: u64) -> Self {
        Self(nanos)
    }

    #[must_use]
    pub const fn as_nanos(self) -> u64 {
        self.0
    }

    /// The time `duration` after this one.
    #[must_use]
    pub fn saturating_add(self, duration: Duration) -> Self {
        Self(
            self.0
                .saturating_add(u64::try_from(duration.as_nanos()).unwrap_or(u64::MAX)),
        )
    }

    /// How long after `earlier` this time is, or zero if it is not after it.
    #[must_use]
    pub fn saturating_duration_since(self, earlier: Self) -> Duration {
        Duration::from_nanos(self.0.saturating_sub(earlier.0))
    }
}

/// Read `CLOCK_MONOTONIC`.
#[must_use]
pub fn monotonic_now() -> MonotonicTime {
    let mut now = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // SAFETY: `now` is a valid, writable `timespec` for the duration of the
    // call, and `clock_gettime` writes nothing else.
    let result = unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &raw mut now) };
    // CLOCK_MONOTONIC is always available on Linux; a failure leaves `now`
    // at zero, which reads as a very old time stamp rather than a panic.
    debug_assert_eq!(result, 0, "CLOCK_MONOTONIC is unavailable");
    let seconds = u64::try_from(now.tv_sec).unwrap_or(0);
    let nanos = u64::try_from(now.tv_nsec).unwrap_or(0);
    MonotonicTime(seconds.saturating_mul(1_000_000_000).saturating_add(nanos))
}

/// Where the engine reads the time for its position samples. Tests give
/// the engine one they control.
pub type MonotonicClock = Arc<dyn Fn() -> MonotonicTime + Send + Sync>;

/// The clock the engine uses outside tests.
#[must_use]
pub fn system_clock() -> MonotonicClock {
    Arc::new(monotonic_now)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn monotonic_now_reads_a_clock_that_does_not_go_back() {
        let first = monotonic_now();
        let second = monotonic_now();
        assert!(first > MonotonicTime::default());
        assert!(second >= first);
    }

    #[test]
    fn monotonic_time_saturates() {
        let time = MonotonicTime::from_nanos(10);
        assert_eq!(
            time.saturating_add(Duration::from_nanos(5)),
            MonotonicTime::from_nanos(15)
        );
        assert_eq!(
            MonotonicTime::from_nanos(u64::MAX).saturating_add(Duration::from_secs(1)),
            MonotonicTime::from_nanos(u64::MAX)
        );
        assert_eq!(
            time.saturating_duration_since(MonotonicTime::from_nanos(20)),
            Duration::ZERO
        );
        assert_eq!(
            time.saturating_duration_since(MonotonicTime::from_nanos(4)),
            Duration::from_nanos(6)
        );
    }
}
