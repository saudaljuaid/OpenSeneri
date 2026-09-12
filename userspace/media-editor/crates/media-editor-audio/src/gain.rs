// SPDX-License-Identifier: GPL-3.0-only
//! Gain, in the units a mixer is actually labelled in.
//!
//! A fader is marked in decibels because hearing is logarithmic, and a decibel
//! of amplitude is `20 · log10(factor)`. That is a logarithm, so a mixer needs
//! one — and on Phipia there is no libm to ask, which turns out to be the
//! useful constraint rather than the awkward one. [`media_editor_core::Fixed`]
//! computes it with integers, so two machines that mix the same session
//! produce the same samples, for ever (R-4.1).
//!
//! Three values anchor the scale and none of them is approximate:
//!
//! - **zero decibels is exactly one.** A fader at unity must pass its input
//!   through untouched. A mixer whose unity is 0.99997 colours every channel
//!   it touches, and the error compounds down a bus.
//! - **twenty decibels is exactly ten.** That is what the definition says:
//!   `20 · log10(10) = 20`. It is the cleanest available check that the
//!   logarithm and the exponential are inverses of each other over a wide
//!   span, and it is asserted at ±20, ±40 and ±60.
//! - **six decibels is about double**, and the "about" is the point: it is
//!   `6.0206`, not `6`. Rounding it to six is how a gain staging chart ends up
//!   a third of a decibel out over five stages.

use media_editor_core::{CoreStatus, Fixed, Rational};

use crate::status::{AudioStatus, Result};

/// The quietest gain that is not silence, in decibels.
///
/// Minus one hundred and twenty: below the noise floor of any converter ever
/// built, and far enough down that anything quieter is silence in every sense
/// that matters. It is a bound rather than a taste, so that a hostile session
/// file cannot ask for a factor no arithmetic can hold (R-11.2).
pub const MINIMUM_DECIBELS: i64 = -120;

/// The loudest gain a fader may be set to, in decibels.
///
/// Twenty-four, which is what a large-format console offers and more than any
/// well-recorded source needs. A channel that wants more than this wants
/// looking at rather than turning up.
pub const MAXIMUM_DECIBELS: i64 = 24;

/// A gain, held as the exact decibel value it was set to.
///
/// The decibels are kept rather than the factor, because they are what the
/// user set and what the interface shows. Converting to a factor is a lossy
/// step through a logarithm, and doing it once at the edge means a fader moved
/// and moved back is exactly where it started — rather than a few bits away
/// from it, which is what storing the factor would give.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Gain {
    decibels: Rational,
    silent: bool,
}

impl Gain {
    /// A fader at unity: the input, untouched.
    pub const UNITY: Self = Self {
        decibels: Rational::ZERO,
        silent: false,
    };

    /// Silence, which is not a decibel value at all.
    ///
    /// The logarithm of zero is not a number, so "off" cannot be written on
    /// the decibel scale — every real fader has a separate detent below its
    /// lowest marking, and so does this.
    pub const SILENT: Self = Self {
        decibels: Rational::ZERO,
        silent: true,
    };

    /// A gain in decibels.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::GainOutOfRange`] outside [`MINIMUM_DECIBELS`] to
    /// [`MAXIMUM_DECIBELS`].
    pub fn decibels(value: Rational) -> Result<Self> {
        let low = Rational::new(MINIMUM_DECIBELS, 1)?;
        let high = Rational::new(MAXIMUM_DECIBELS, 1)?;
        if value < low || value > high {
            return Err(AudioStatus::GainOutOfRange);
        }
        Ok(Self {
            decibels: value,
            silent: false,
        })
    }

    /// A gain in whole decibels.
    ///
    /// # Errors
    ///
    /// As [`Gain::decibels`].
    pub fn whole_decibels(value: i64) -> Result<Self> {
        Self::decibels(Rational::new(value, 1)?)
    }

    /// Whether this is the off detent rather than a level.
    #[must_use]
    pub const fn is_silent(self) -> bool {
        self.silent
    }

    /// The decibel value, or `None` for silence.
    #[must_use]
    pub const fn value(self) -> Option<Rational> {
        if self.silent {
            None
        } else {
            Some(self.decibels)
        }
    }

    /// The linear factor this gain multiplies a sample by.
    ///
    /// `10^(decibels / 20)`, computed with integers.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::Time`] wrapping an arithmetic refusal.
    pub fn factor(self) -> Result<Fixed> {
        if self.silent {
            return Ok(Fixed::ZERO);
        }
        if self.decibels.is_zero() {
            // Unity is common and does not need the logarithmic general path.
            // The general path is tested separately to return the same value.
            return Ok(Fixed::ONE);
        }
        let exponent = Fixed::from_rational(self.decibels.checked_div(Rational::new(20, 1)?)?)?;
        Ok(Fixed::from_integer(10)?.pow(exponent)?)
    }

    /// The gain that undoes this one.
    ///
    /// Silence has no inverse: nothing multiplied by zero comes back.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::NoInverse`] for silence, or
    /// [`AudioStatus::GainOutOfRange`] if the opposite is past the bounds.
    pub fn inverse(self) -> Result<Self> {
        if self.silent {
            return Err(AudioStatus::NoInverse);
        }
        Self::decibels(self.decibels.checked_neg()?)
    }

    /// Two gains applied one after the other.
    ///
    /// Decibels add, which is the whole reason the scale is logarithmic: a
    /// channel fader and a bus fader in series are their sum, computed exactly
    /// rather than by multiplying two rounded factors.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::GainOutOfRange`] if the sum leaves the bounds.
    pub fn then(self, other: Self) -> Result<Self> {
        if self.silent || other.silent {
            return Ok(Self::SILENT);
        }
        Self::decibels(self.decibels.checked_add(other.decibels)?)
    }
}

impl From<CoreStatus> for AudioStatus {
    fn from(status: CoreStatus) -> Self {
        Self::Time(status)
    }
}
