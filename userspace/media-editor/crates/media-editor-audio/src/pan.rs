// SPDX-License-Identifier: GPL-3.0-only
//! Constant-power stereo panning.
//!
//! For a normalized position `p`, gains are `√(1 - p)` and `√p`, so
//! `left² + right² = 1`. Centre is `√½` on both channels, about -3.01 dB.
//! [`media_editor_core::Fixed`] provides the integer square root, avoiding
//! floating point and lookup tables.

use media_editor_core::{Fixed, Rational};

use crate::status::{AudioStatus, Result};

/// Where a source sits between the speakers.
///
/// Minus one is hard left, zero is centre, plus one is hard right. Held as an
/// exact rational, because a pan moved and moved back must land where it
/// started.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Pan {
    position: Rational,
}

impl Pan {
    /// Dead centre.
    pub const CENTRE: Self = Self {
        position: Rational::ZERO,
    };

    /// A position between hard left and hard right.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::PanOutOfRange`] outside minus one to plus one.
    pub fn new(position: Rational) -> Result<Self> {
        let one = Rational::ONE;
        if position < one.checked_neg()? || position > one {
            return Err(AudioStatus::PanOutOfRange);
        }
        Ok(Self { position })
    }

    /// Hard left.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::Time`] wrapping an arithmetic refusal.
    pub fn left() -> Result<Self> {
        Self::new(Rational::ONE.checked_neg()?)
    }

    /// Hard right.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::Time`] wrapping an arithmetic refusal.
    pub fn right() -> Result<Self> {
        Self::new(Rational::ONE)
    }

    /// Where this sits.
    #[must_use]
    pub const fn position(self) -> Rational {
        self.position
    }

    /// The mirror image of this position.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::Time`] wrapping an arithmetic refusal.
    pub fn mirrored(self) -> Result<Self> {
        Self::new(self.position.checked_neg()?)
    }

    /// The gains this position sends to the left and right speakers.
    ///
    /// Constant power: the squares sum to one at every position, so moving a
    /// source across the image does not change how loud it is.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::Time`] wrapping an arithmetic refusal.
    pub fn gains(self) -> Result<(Fixed, Fixed)> {
        // The position runs from minus one to plus one; the fraction of the
        // way across runs from zero to one.
        let two = Rational::new(2, 1)?;
        let fraction = self.position.checked_add(Rational::ONE)?.checked_div(two)?;
        let right = Fixed::from_rational(fraction)?;
        let left = Fixed::ONE.checked_sub(right)?;
        Ok((left.sqrt()?, right.sqrt()?))
    }
}
