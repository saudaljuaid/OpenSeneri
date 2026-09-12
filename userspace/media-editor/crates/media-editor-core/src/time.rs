// SPDX-License-Identifier: GPL-3.0-only
//! Positions and lengths on a timeline.
//!
//! An [`Instant`] is a position; a [`Duration`] is a length. Both carry their
//! [`Timebase`], and neither combines with a quantity in a different one. A
//! conversion between timebases is exact or it is refused — never rounded,
//! because a rounded frame position is a frame the editor did not ask for.
//!
//! Durations are non-negative. A signed difference is not a duration; it is
//! two instants and a question about their order.

use core::cmp::Ordering;

use crate::rational::Rational;
use crate::status::{CoreStatus, Result};
use crate::timebase::Timebase;

/// A position, counted in ticks of a timebase.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Instant {
    ticks: i64,
    timebase: Timebase,
}

/// A length, counted in ticks of a timebase. Never negative.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Duration {
    ticks: i64,
    timebase: Timebase,
}

impl Instant {
    /// A position at an exact tick count.
    #[must_use]
    pub const fn new(ticks: i64, timebase: Timebase) -> Self {
        Self { ticks, timebase }
    }

    /// Tick zero in a timebase.
    #[must_use]
    pub const fn origin(timebase: Timebase) -> Self {
        Self::new(0, timebase)
    }

    /// The tick count.
    #[must_use]
    pub const fn ticks(self) -> i64 {
        self.ticks
    }

    /// The timebase this is counted in.
    #[must_use]
    pub const fn timebase(self) -> Timebase {
        self.timebase
    }

    /// This position moved later by a duration.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::TimebaseMismatch`] if the timebases differ, and
    /// [`CoreStatus::Overflow`] if the sum does not fit.
    pub fn advance(self, duration: Duration) -> Result<Self> {
        if self.timebase != duration.timebase {
            return Err(CoreStatus::TimebaseMismatch);
        }
        let ticks = self
            .ticks
            .checked_add(duration.ticks)
            .ok_or(CoreStatus::Overflow)?;
        Ok(Self::new(ticks, self.timebase))
    }

    /// This position moved earlier by a duration.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::TimebaseMismatch`] if the timebases differ, and
    /// [`CoreStatus::Overflow`] if the difference does not fit.
    pub fn retreat(self, duration: Duration) -> Result<Self> {
        if self.timebase != duration.timebase {
            return Err(CoreStatus::TimebaseMismatch);
        }
        let ticks = self
            .ticks
            .checked_sub(duration.ticks)
            .ok_or(CoreStatus::Overflow)?;
        Ok(Self::new(ticks, self.timebase))
    }

    /// The length from an earlier position to this one.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::TimebaseMismatch`] if the timebases differ,
    /// [`CoreStatus::NegativeDuration`] if `earlier` is later than this, and
    /// [`CoreStatus::Overflow`] if the difference does not fit.
    pub fn since(self, earlier: Self) -> Result<Duration> {
        if self.timebase != earlier.timebase {
            return Err(CoreStatus::TimebaseMismatch);
        }
        let ticks = self
            .ticks
            .checked_sub(earlier.ticks)
            .ok_or(CoreStatus::Overflow)?;
        Duration::new(ticks, self.timebase)
    }

    /// Compare two positions in the same timebase.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::TimebaseMismatch`] if the timebases differ. Positions in
    /// different timebases are converted before they are compared, never
    /// compared by their raw tick counts.
    pub fn compare(self, other: Self) -> Result<Ordering> {
        if self.timebase != other.timebase {
            return Err(CoreStatus::TimebaseMismatch);
        }
        Ok(self.ticks.cmp(&other.ticks))
    }

    /// This position in seconds, exactly.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`] if the exact value does not fit.
    pub fn seconds(self) -> Result<Rational> {
        Rational::from_integer(self.ticks).checked_div(self.timebase.rate())
    }

    /// This position in another timebase, exactly.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::InexactConversion`] if the position does not fall on a
    /// tick of the target timebase, and [`CoreStatus::Overflow`] if the exact
    /// value does not fit.
    pub fn convert(self, target: Timebase) -> Result<Self> {
        Ok(Self::new(
            convert_ticks(self.ticks, self.timebase, target)?,
            target,
        ))
    }

    /// Convert this position to the containing tick in another timebase.
    ///
    /// Unlike [`Instant::convert`], this accepts positions between target ticks
    /// and rounds down. Consecutive boundaries therefore partition target ticks
    /// without shifting a start past its true position. The separate method
    /// keeps exact conversion and containment conversion explicit (R-1.3).
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`] if the value does not fit, or
    /// [`CoreStatus::NonPositiveRate`] wrapped from the arithmetic.
    pub fn floor_into(self, target: Timebase) -> Result<Self> {
        // ticks x (target rate / source rate), floored. Done as one exact
        // rational and floored once, rather than as two divisions, so nothing
        // rounds twice.
        let scaled = self
            .seconds()?
            .checked_mul(target.rate())
            .map_err(|_| CoreStatus::Overflow)?;
        let numerator = scaled.numerator();
        let denominator = scaled.denominator();
        // Rust's integer division truncates towards zero, so a negative
        // position would round the wrong way — towards the origin rather than
        // downwards — and a clip before zero would start one tick late.
        let floored = if numerator >= 0 {
            numerator / denominator
        } else {
            -((-numerator + denominator - 1) / denominator)
        };
        Ok(Self::new(floored, target))
    }
}

impl Duration {
    /// A length of an exact number of ticks.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::NegativeDuration`] if the count is negative.
    pub fn new(ticks: i64, timebase: Timebase) -> Result<Self> {
        if ticks < 0 {
            return Err(CoreStatus::NegativeDuration);
        }
        Ok(Self { ticks, timebase })
    }

    /// A zero-length duration in a timebase.
    #[must_use]
    pub const fn zero(timebase: Timebase) -> Self {
        Self { ticks: 0, timebase }
    }

    /// The tick count.
    #[must_use]
    pub const fn ticks(self) -> i64 {
        self.ticks
    }

    /// The timebase this is counted in.
    #[must_use]
    pub const fn timebase(self) -> Timebase {
        self.timebase
    }

    /// Whether this length is zero.
    #[must_use]
    pub const fn is_zero(self) -> bool {
        self.ticks == 0
    }

    /// The sum of two lengths.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::TimebaseMismatch`] or [`CoreStatus::Overflow`].
    pub fn checked_add(self, other: Self) -> Result<Self> {
        if self.timebase != other.timebase {
            return Err(CoreStatus::TimebaseMismatch);
        }
        let ticks = self
            .ticks
            .checked_add(other.ticks)
            .ok_or(CoreStatus::Overflow)?;
        Self::new(ticks, self.timebase)
    }

    /// The difference of two lengths.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::TimebaseMismatch`], [`CoreStatus::NegativeDuration`] if
    /// the result would be negative, or [`CoreStatus::Overflow`].
    pub fn checked_sub(self, other: Self) -> Result<Self> {
        if self.timebase != other.timebase {
            return Err(CoreStatus::TimebaseMismatch);
        }
        let ticks = self
            .ticks
            .checked_sub(other.ticks)
            .ok_or(CoreStatus::Overflow)?;
        Self::new(ticks, self.timebase)
    }

    /// This length extended or shortened by a signed number of ticks.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::NegativeDuration`] if the result would be negative, or
    /// [`CoreStatus::Overflow`].
    pub fn offset(self, delta: i64) -> Result<Self> {
        let ticks = self.ticks.checked_add(delta).ok_or(CoreStatus::Overflow)?;
        Self::new(ticks, self.timebase)
    }

    /// Compare two lengths in the same timebase.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::TimebaseMismatch`] if the timebases differ.
    pub fn compare(self, other: Self) -> Result<Ordering> {
        if self.timebase != other.timebase {
            return Err(CoreStatus::TimebaseMismatch);
        }
        Ok(self.ticks.cmp(&other.ticks))
    }

    /// This length in seconds, exactly.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`] if the exact value does not fit.
    pub fn seconds(self) -> Result<Rational> {
        Rational::from_integer(self.ticks).checked_div(self.timebase.rate())
    }

    /// This length in another timebase, exactly.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::InexactConversion`] or [`CoreStatus::Overflow`].
    pub fn convert(self, target: Timebase) -> Result<Self> {
        Self::new(convert_ticks(self.ticks, self.timebase, target)?, target)
    }
}

/// Convert a tick count between two timebases, exactly or not at all.
///
/// The count in the target timebase is `ticks * target_rate / source_rate`.
/// Both rates are reduced positive fractions, so the whole expression is one
/// exact ratio of integers, and the conversion succeeds only when that ratio
/// is a whole number.
fn convert_ticks(ticks: i64, source: Timebase, target: Timebase) -> Result<i64> {
    if source == target {
        return Ok(ticks);
    }
    let source_rate = source.rate();
    let target_rate = target.rate();
    let numerator = i128::from(ticks)
        .checked_mul(i128::from(target_rate.numerator()))
        .and_then(|value| value.checked_mul(i128::from(source_rate.denominator())))
        .ok_or(CoreStatus::Overflow)?;
    let denominator = i128::from(target_rate.denominator())
        .checked_mul(i128::from(source_rate.numerator()))
        .ok_or(CoreStatus::Overflow)?;
    if denominator == 0 {
        return Err(CoreStatus::ZeroDenominator);
    }
    if numerator % denominator != 0 {
        return Err(CoreStatus::InexactConversion);
    }
    i64::try_from(numerator / denominator).map_err(|_| CoreStatus::Overflow)
}
