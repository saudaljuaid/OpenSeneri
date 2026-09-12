// SPDX-License-Identifier: GPL-3.0-only
//! Half-open intervals of time.
//!
//! Every range in Media Editor is `[start, start + duration)`: the first instant
//! is included and the instant one tick past the end is not. There is no
//! closed range anywhere in the project, because the two conventions cannot
//! coexist without producing off-by-one errors at every edit point.

use core::cmp::Ordering;

use crate::status::{CoreStatus, Result};
use crate::time::{Duration, Instant};
use crate::timebase::Timebase;

/// A half-open interval `[start, end)`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct TimeRange {
    start: Instant,
    duration: Duration,
}

impl TimeRange {
    /// A range beginning at `start` and lasting `duration`.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::TimebaseMismatch`] if the two are counted differently.
    pub fn new(start: Instant, duration: Duration) -> Result<Self> {
        if start.timebase() != duration.timebase() {
            return Err(CoreStatus::TimebaseMismatch);
        }
        // Prove the end is representable now rather than at every later use.
        start.advance(duration)?;
        Ok(Self { start, duration })
    }

    /// A range from one instant up to, but not including, another.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::TimebaseMismatch`] or [`CoreStatus::RangeMalformed`] if
    /// the end precedes the start.
    pub fn between(start: Instant, end: Instant) -> Result<Self> {
        if start.timebase() != end.timebase() {
            return Err(CoreStatus::TimebaseMismatch);
        }
        let duration = end.since(start).map_err(|status| match status {
            CoreStatus::NegativeDuration => CoreStatus::RangeMalformed,
            other => other,
        })?;
        Self::new(start, duration)
    }

    /// An empty range at an instant.
    #[must_use]
    pub fn empty_at(start: Instant) -> Self {
        Self {
            start,
            duration: Duration::zero(start.timebase()),
        }
    }

    /// The first instant inside the range.
    #[must_use]
    pub const fn start(self) -> Instant {
        self.start
    }

    /// The length of the range.
    #[must_use]
    pub const fn duration(self) -> Duration {
        self.duration
    }

    /// The timebase both parts are counted in.
    #[must_use]
    pub const fn timebase(self) -> Timebase {
        self.start.timebase()
    }

    /// The first instant past the range.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`] if the end is not representable, which
    /// [`TimeRange::new`] has already refused.
    pub fn end(self) -> Result<Instant> {
        self.start.advance(self.duration)
    }

    /// Whether the range contains no instants.
    #[must_use]
    pub const fn is_empty(self) -> bool {
        self.duration.is_zero()
    }

    /// Whether an instant falls inside the range.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::TimebaseMismatch`] if the instant is counted differently.
    pub fn contains(self, instant: Instant) -> Result<bool> {
        let after_start = instant.compare(self.start)? != Ordering::Less;
        let before_end = instant.compare(self.end()?)? == Ordering::Less;
        Ok(after_start && before_end)
    }

    /// Whether two ranges share at least one instant.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::TimebaseMismatch`] if the ranges are counted differently.
    pub fn intersects(self, other: Self) -> Result<bool> {
        Ok(self
            .intersection(other)?
            .is_some_and(|range| !range.is_empty()))
    }

    /// The overlap of two ranges, if they overlap at all.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::TimebaseMismatch`] if the ranges are counted differently.
    pub fn intersection(self, other: Self) -> Result<Option<Self>> {
        if self.timebase() != other.timebase() {
            return Err(CoreStatus::TimebaseMismatch);
        }
        let start = if self.start.compare(other.start)? == Ordering::Greater {
            self.start
        } else {
            other.start
        };
        let end = if self.end()?.compare(other.end()?)? == Ordering::Less {
            self.end()?
        } else {
            other.end()?
        };
        if end.compare(start)? == Ordering::Less {
            return Ok(None);
        }
        Ok(Some(Self::between(start, end)?))
    }

    /// This range in another timebase, exactly.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::InexactConversion`] if either edge does not fall on a
    /// tick of the target timebase.
    pub fn convert(self, target: Timebase) -> Result<Self> {
        Self::new(self.start.convert(target)?, self.duration.convert(target)?)
    }
}
