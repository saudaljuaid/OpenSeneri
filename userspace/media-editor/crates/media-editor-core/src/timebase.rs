// SPDX-License-Identifier: GPL-3.0-only
//! The rate a position is counted in.
//!
//! A timebase is ticks per second, exactly. Every instant and every duration
//! carries the timebase it is counted in, and two quantities in different
//! timebases do not combine without an explicit conversion that is either
//! exact or refused (R-4.8).

use crate::rational::Rational;
use crate::status::{CoreStatus, Result};

/// Ticks per second, exactly.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Timebase {
    rate: Rational,
}

impl Timebase {
    /// Twenty-four frames per second: cinema.
    pub const FILM_24: Self = Self::from_reduced(24, 1);
    /// Twenty-five frames per second: PAL and most of the world's television.
    pub const PAL_25: Self = Self::from_reduced(25, 1);
    /// Thirty frames per second, exactly.
    pub const TELEVISION_30: Self = Self::from_reduced(30, 1);
    /// Fifty frames per second, exactly.
    pub const PAL_50: Self = Self::from_reduced(50, 1);
    /// Sixty frames per second, exactly.
    pub const TELEVISION_60: Self = Self::from_reduced(60, 1);
    /// 24000/1001: film transferred for NTSC. Never 23.976.
    pub const NTSC_FILM: Self = Self::from_reduced(24_000, 1001);
    /// 30000/1001: NTSC video. Never 29.97.
    pub const NTSC_30: Self = Self::from_reduced(30_000, 1001);
    /// 60000/1001: NTSC at double rate. Never 59.94.
    pub const NTSC_60: Self = Self::from_reduced(60_000, 1001);
    /// Forty-eight thousand samples per second: the audio rate for picture.
    pub const AUDIO_48K: Self = Self::from_reduced(48_000, 1);
    /// Forty-four thousand one hundred samples per second.
    pub const AUDIO_44K1: Self = Self::from_reduced(44_100, 1);
    /// Eighty-eight thousand two hundred samples per second: double 44.1.
    pub const AUDIO_88K2: Self = Self::from_reduced(88_200, 1);
    /// Ninety-six thousand samples per second: double 48.
    pub const AUDIO_96K: Self = Self::from_reduced(96_000, 1);
    /// Ninety thousand ticks per second: the MPEG transport timebase.
    pub const MPEG_90K: Self = Self::from_reduced(90_000, 1);
    /// One nanosecond ticks, for interoperating with a monotonic clock.
    pub const NANOSECONDS: Self = Self::from_reduced(1_000_000_000, 1);

    /// Build a timebase from an already reduced, positive fraction.
    ///
    /// Private, and only ever used to define the constants above. The asserts
    /// are evaluated during constant evaluation, so a mistake in one of those
    /// definitions is a compile error rather than a runtime panic, and no
    /// public path can reach them (R-3.1.5).
    const fn from_reduced(numerator: i64, denominator: i64) -> Self {
        assert!(numerator > 0, "a timebase rate must be positive");
        assert!(denominator > 0, "a timebase denominator must be positive");
        let mut left = numerator;
        let mut right = denominator;
        while right != 0 {
            let remainder = left % right;
            left = right;
            right = remainder;
        }
        assert!(left == 1, "a timebase constant must already be reduced");
        Self {
            rate: match Rational::new_const(numerator, denominator) {
                Some(rate) => rate,
                None => panic!("a reduced positive fraction is representable"),
            },
        }
    }

    /// Build a timebase from a rate in ticks per second.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::NonPositiveRate`] if the rate is zero or negative.
    pub fn new(rate: Rational) -> Result<Self> {
        if !rate.is_positive() {
            return Err(CoreStatus::NonPositiveRate);
        }
        Ok(Self { rate })
    }

    /// Build a timebase from a fraction of ticks per second.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::ZeroDenominator`] or [`CoreStatus::NonPositiveRate`].
    pub fn from_fraction(numerator: i64, denominator: i64) -> Result<Self> {
        Self::new(Rational::new(numerator, denominator)?)
    }

    /// The rate, in ticks per second.
    #[must_use]
    pub const fn rate(self) -> Rational {
        self.rate
    }

    /// The nominal whole-number rate, as broadcast practice names it: 24 for
    /// both 24 and 24000/1001, 30 for both 30 and 30000/1001.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::UnsupportedTimecodeRate`] if the rate does not round to a
    /// whole number of ticks per second, which means timecode is undefined for
    /// it.
    pub fn nominal_rate(self) -> Result<u32> {
        // Round to nearest: 24000/1001 is 23.976..., whose nominal rate is 24.
        let doubled = self
            .rate
            .scale(2)?
            .checked_add(Rational::ONE)?
            .checked_div(Rational::from_integer(2))?;
        let rounded = doubled.floor()?;
        u32::try_from(rounded).map_err(|_| CoreStatus::UnsupportedTimecodeRate)
    }

    /// Whether this timebase counts in whole ticks per second.
    #[must_use]
    pub fn is_integral(self) -> bool {
        self.rate.to_integer().is_some()
    }
}

impl core::fmt::Display for Timebase {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(formatter, "{}", self.rate)
    }
}
