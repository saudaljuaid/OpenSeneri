// SPDX-License-Identifier: GPL-3.0-only
//! Exact rational arithmetic.
//!
//! Time in Media Editor is rational and never floating point (R-4.8). A frame
//! rate of 24000/1001 is that fraction; it is never 23.976, because 23.976 is
//! a different number and the difference accumulates into a drift the editor
//! would be blamed for.
//!
//! Every value is normalised on construction: the denominator is positive and
//! the two parts share no common factor. That makes equality structural and
//! makes comparison a single cross-multiplication.
//!
//! Every operation is checked. Intermediate products are computed in `i128`,
//! which cannot overflow for `i64` inputs, and a result that does not fit back
//! into `i64` is refused rather than truncated.

use core::cmp::Ordering;
use core::num::NonZeroI64;

use crate::status::{CoreStatus, Result};

/// An exact ratio of two integers, always in lowest terms with a positive
/// denominator.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Rational {
    numerator: i64,
    denominator: NonZeroI64,
}

/// One, as a non-zero denominator, in a const context.
const ONE_DENOMINATOR: NonZeroI64 = match NonZeroI64::new(1) {
    Some(one) => one,
    None => panic!("one is not zero"),
};

/// Two, as a non-zero denominator, in a const context.
const TWO_DENOMINATOR: NonZeroI64 = match NonZeroI64::new(2) {
    Some(two) => two,
    None => panic!("two is not zero"),
};

impl Rational {
    /// Zero.
    pub const ZERO: Self = Self {
        numerator: 0,
        denominator: ONE_DENOMINATOR,
    };

    /// One.
    pub const ONE: Self = Self {
        numerator: 1,
        denominator: ONE_DENOMINATOR,
    };

    /// A half.
    ///
    /// Here rather than at each call site because it is the middle of a frame,
    /// and a `const` cannot call [`Rational::new`] to find that out — the
    /// reduction is a loop. Already reduced: two is even and one is not.
    pub const HALF: Self = Self {
        numerator: 1,
        denominator: TWO_DENOMINATOR,
    };

    /// Build a rational from a numerator and denominator, reducing it.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::ZeroDenominator`] if the denominator is zero, and
    /// [`CoreStatus::Overflow`] if the reduced form does not fit.
    pub fn new(numerator: i64, denominator: i64) -> Result<Self> {
        if denominator == 0 {
            return Err(CoreStatus::ZeroDenominator);
        }
        Self::from_wide(i128::from(numerator), i128::from(denominator))
    }

    /// Build a rational equal to an integer.
    #[must_use]
    pub const fn from_integer(value: i64) -> Self {
        Self {
            numerator: value,
            denominator: ONE_DENOMINATOR,
        }
    }

    /// Build a rational from a fraction that is already reduced and has a
    /// positive denominator, in a constant context.
    ///
    /// Returns `None` for anything else rather than reducing it, so that a
    /// constant defined with an unreduced fraction fails to compile instead of
    /// quietly differing from its written form.
    pub(crate) const fn new_const(numerator: i64, denominator: i64) -> Option<Self> {
        if denominator <= 0 {
            return None;
        }
        match NonZeroI64::new(denominator) {
            Some(denominator) => Some(Self {
                numerator,
                denominator,
            }),
            None => None,
        }
    }

    /// Reduce a wide pair into a normalised rational, or refuse it.
    fn from_wide(mut numerator: i128, mut denominator: i128) -> Result<Self> {
        if denominator == 0 {
            return Err(CoreStatus::ZeroDenominator);
        }
        if denominator < 0 {
            numerator = -numerator;
            denominator = -denominator;
        }
        let divisor = gcd(numerator.unsigned_abs(), denominator.unsigned_abs());
        let divisor = i128::try_from(divisor).map_err(|_| CoreStatus::Overflow)?;
        // `divisor` is the greatest common divisor of a pair whose second
        // member is non-zero, so it is non-zero and divides both exactly.
        numerator /= divisor;
        denominator /= divisor;
        let numerator = i64::try_from(numerator).map_err(|_| CoreStatus::Overflow)?;
        let denominator = i64::try_from(denominator).map_err(|_| CoreStatus::Overflow)?;
        let denominator = NonZeroI64::new(denominator).ok_or(CoreStatus::ZeroDenominator)?;
        Ok(Self {
            numerator,
            denominator,
        })
    }

    /// The numerator of the reduced form.
    #[must_use]
    pub const fn numerator(self) -> i64 {
        self.numerator
    }

    /// The denominator of the reduced form, always positive.
    #[must_use]
    pub const fn denominator(self) -> i64 {
        self.denominator.get()
    }

    /// Whether this is exactly zero.
    #[must_use]
    pub const fn is_zero(self) -> bool {
        self.numerator == 0
    }

    /// Whether this is greater than zero.
    #[must_use]
    pub const fn is_positive(self) -> bool {
        self.numerator > 0
    }

    /// The integer this equals, if it is an integer.
    #[must_use]
    pub const fn to_integer(self) -> Option<i64> {
        if self.denominator.get() == 1 {
            Some(self.numerator)
        } else {
            None
        }
    }

    /// The sum of two rationals.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`] if the exact sum does not fit.
    pub fn checked_add(self, other: Self) -> Result<Self> {
        // Over the common denominator rather than over the product of the two.
        // The product is the obvious expression and it is the one that
        // overflows: adding three matrix elements whose denominators share
        // most of their factors would otherwise square those factors twice
        // before anything reduced them. Colour work chains enough of these
        // that the difference decides whether an exact answer exists.
        let left_denominator = i128::from(self.denominator());
        let right_denominator = i128::from(other.denominator());
        let common = i128::try_from(gcd(
            left_denominator.unsigned_abs(),
            right_denominator.unsigned_abs(),
        ))
        .map_err(|_| CoreStatus::Overflow)?;
        let right_scaled = right_denominator / common;
        let left_scaled = left_denominator / common;

        let left = i128::from(self.numerator)
            .checked_mul(right_scaled)
            .ok_or(CoreStatus::Overflow)?;
        let right = i128::from(other.numerator)
            .checked_mul(left_scaled)
            .ok_or(CoreStatus::Overflow)?;
        let numerator = left.checked_add(right).ok_or(CoreStatus::Overflow)?;
        let denominator = left_denominator
            .checked_mul(right_scaled)
            .ok_or(CoreStatus::Overflow)?;
        Self::from_wide(numerator, denominator)
    }

    /// The difference of two rationals.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`] if the exact difference does not fit.
    pub fn checked_sub(self, other: Self) -> Result<Self> {
        self.checked_add(other.checked_neg()?)
    }

    /// The product of two rationals.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`] if the exact product does not fit.
    pub fn checked_mul(self, other: Self) -> Result<Self> {
        // Cancel across the two fractions before multiplying, for the same
        // reason: the factors that would cancel afterwards are exactly the
        // ones that make the intermediate too wide to represent.
        let left_numerator = i128::from(self.numerator);
        let left_denominator = i128::from(self.denominator());
        let right_numerator = i128::from(other.numerator);
        let right_denominator = i128::from(other.denominator());

        let first = i128::try_from(gcd(
            left_numerator.unsigned_abs(),
            right_denominator.unsigned_abs(),
        ))
        .map_err(|_| CoreStatus::Overflow)?;
        let second = i128::try_from(gcd(
            right_numerator.unsigned_abs(),
            left_denominator.unsigned_abs(),
        ))
        .map_err(|_| CoreStatus::Overflow)?;

        let numerator = (left_numerator / first)
            .checked_mul(right_numerator / second)
            .ok_or(CoreStatus::Overflow)?;
        let denominator = (left_denominator / second)
            .checked_mul(right_denominator / first)
            .ok_or(CoreStatus::Overflow)?;
        Self::from_wide(numerator, denominator)
    }

    /// The quotient of two rationals.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::DivideByZero`] if the divisor is zero, and
    /// [`CoreStatus::Overflow`] if the exact quotient does not fit.
    pub fn checked_div(self, other: Self) -> Result<Self> {
        if other.is_zero() {
            return Err(CoreStatus::DivideByZero);
        }
        self.checked_mul(other.checked_reciprocal()?)
    }

    /// This value with its sign flipped.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`] if the numerator is `i64::MIN`.
    pub fn checked_neg(self) -> Result<Self> {
        let numerator = self.numerator.checked_neg().ok_or(CoreStatus::Overflow)?;
        Ok(Self {
            numerator,
            denominator: self.denominator,
        })
    }

    /// One divided by this value.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::DivideByZero`] if this is zero, and
    /// [`CoreStatus::Overflow`] if the result does not fit.
    pub fn checked_reciprocal(self) -> Result<Self> {
        if self.is_zero() {
            return Err(CoreStatus::DivideByZero);
        }
        Self::from_wide(i128::from(self.denominator()), i128::from(self.numerator))
    }

    /// This value multiplied by an integer.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`] if the exact product does not fit.
    pub fn scale(self, factor: i64) -> Result<Self> {
        self.checked_mul(Self::from_integer(factor))
    }

    /// The largest integer no greater than this value.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`] if the floor does not fit in an `i64`.
    pub fn floor(self) -> Result<i64> {
        let numerator = i128::from(self.numerator);
        let denominator = i128::from(self.denominator());
        // The divisor is positive, so Euclidean division already rounds toward
        // negative infinity, which is what a floor is.
        i64::try_from(numerator.div_euclid(denominator)).map_err(|_| CoreStatus::Overflow)
    }
}

impl PartialOrd for Rational {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for Rational {
    fn cmp(&self, other: &Self) -> Ordering {
        // Both denominators are positive, so cross-multiplication preserves the
        // ordering, and the product of two `i64` values always fits in `i128`.
        let left = i128::from(self.numerator) * i128::from(other.denominator());
        let right = i128::from(other.numerator) * i128::from(self.denominator());
        left.cmp(&right)
    }
}

impl core::fmt::Display for Rational {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        if self.denominator.get() == 1 {
            write!(formatter, "{}", self.numerator)
        } else {
            write!(formatter, "{}/{}", self.numerator, self.denominator)
        }
    }
}

/// The greatest common divisor, by Euclid. Returns one for a pair of zeroes so
/// that the caller's division is always defined.
const fn gcd(mut left: u128, mut right: u128) -> u128 {
    while right != 0 {
        let remainder = left % right;
        left = right;
        right = remainder;
    }
    if left == 0 { 1 } else { left }
}
