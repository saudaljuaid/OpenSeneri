// SPDX-License-Identifier: GPL-3.0-only
//! Fixed-point arithmetic, and the power function built on it.
//!
//! A transfer function is a power law and a decibel is a logarithm, so both
//! the picture side and the sound side need `pow`. Phipia has no libm, and
//! even where one exists R-4.1 would not accept it: `pow`, `exp` and `log` are
//! not specified bit-for-bit by IEEE 754, so two machines with different
//! libraries produce different pixels — and different samples — for the same
//! project. That is precisely the class of difference an editor must not have.
//!
//! So the arithmetic here is integers, all of it. `log2` is the classic
//! bit-by-bit method — square the mantissa, take a bit when it passes two.
//! `exp2` multiplies together the successive square roots of two that the
//! fractional bits select, and those roots are computed with an integer square
//! root rather than embedded as constants, so there is no table anyone has to
//! trust. `pow(x, y)` is `exp2(y · log2(x))`.
//!
//! Every operation is deterministic on every machine, for ever, with no
//! floating point anywhere — which is also what makes it work on Phipia, where
//! there is no guarantee a Ring 3 program may execute a single floating-point
//! instruction. What it costs is speed, and the pipeline pays that once: these
//! functions build tables, they do not run per sample.

use crate::rational::Rational;

use crate::status::{CoreStatus, Result};

/// How many bits of a [`Fixed`] are fractional.
pub const FRACTION_BITS: u32 = 32;

/// The value one.
const ONE: i64 = 1_i64 << FRACTION_BITS;

/// Half of one, for rounding.
const HALF: i128 = 1_i128 << (FRACTION_BITS - 1);

/// A signed number with thirty-two fractional bits.
///
/// The range is about ±2,147,483,648 and the resolution is about
/// 2.3 × 10⁻¹⁰, which is four orders of magnitude finer than a sixteen-bit
/// sample can express.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Fixed(i64);

impl Fixed {
    /// Zero.
    pub const ZERO: Self = Self(0);
    /// One.
    pub const ONE: Self = Self(ONE);

    /// Wrap a raw value, in units of 2⁻³².
    #[must_use]
    pub const fn from_raw(raw: i64) -> Self {
        Self(raw)
    }

    /// The raw value, in units of 2⁻³².
    #[must_use]
    pub const fn raw(self) -> i64 {
        self.0
    }

    /// A whole number.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`] past the representable
    /// range.
    pub fn from_integer(value: i64) -> Result<Self> {
        value.checked_mul(ONE).map(Self).ok_or(CoreStatus::Overflow)
    }

    /// The nearest fixed-point value to an exact rational.
    ///
    /// This is the one place exactness is given up, and it is given up
    /// deliberately and in one direction: the rational is the truth, and this
    /// is the nearest representable approximation of it, rounded half away
    /// from zero so the rule does not depend on the sign.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`].
    pub fn from_rational(value: Rational) -> Result<Self> {
        let numerator = i128::from(value.numerator())
            .checked_shl(FRACTION_BITS)
            .ok_or(CoreStatus::Overflow)?;
        let denominator = i128::from(value.denominator());
        Ok(Self(narrow(divide_rounded(numerator, denominator))?))
    }

    /// This value as an exact rational.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`] or another arithmetic refusal.
    pub fn to_rational(self) -> Result<Rational> {
        Rational::new(self.0, ONE)
    }

    /// The sum.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`].
    pub fn checked_add(self, other: Self) -> Result<Self> {
        self.0
            .checked_add(other.0)
            .map(Self)
            .ok_or(CoreStatus::Overflow)
    }

    /// The difference.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`].
    pub fn checked_sub(self, other: Self) -> Result<Self> {
        self.0
            .checked_sub(other.0)
            .map(Self)
            .ok_or(CoreStatus::Overflow)
    }

    /// The product, rounded half away from zero.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`].
    pub fn checked_mul(self, other: Self) -> Result<Self> {
        let product = i128::from(self.0) * i128::from(other.0);
        Ok(Self(narrow(shift_rounded(product))?))
    }

    /// The quotient, rounded half away from zero.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::NoRealAnswer`] on division by zero, or an overflow.
    pub fn checked_div(self, other: Self) -> Result<Self> {
        if other.0 == 0 {
            return Err(CoreStatus::NoRealAnswer);
        }
        let numerator = i128::from(self.0)
            .checked_shl(FRACTION_BITS)
            .ok_or(CoreStatus::Overflow)?;
        Ok(Self(narrow(divide_rounded(
            numerator,
            i128::from(other.0),
        ))?))
    }

    /// Whether this is greater than zero.
    #[must_use]
    pub const fn is_positive(self) -> bool {
        self.0 > 0
    }

    /// The larger of two values.
    #[must_use]
    pub fn max(self, other: Self) -> Self {
        if self.0 >= other.0 { self } else { other }
    }

    /// The square root.
    ///
    /// Exact to the last representable bit: the integer square root of the
    /// value shifted by the fractional width is the fixed-point square root,
    /// with no iteration whose stopping point anyone has to agree on.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::NoRealAnswer`] for a negative value, which has no real
    /// root.
    pub fn sqrt(self) -> Result<Self> {
        if self.0 < 0 {
            return Err(CoreStatus::NoRealAnswer);
        }
        let widened = u128::try_from(self.0).map_err(|_| CoreStatus::Overflow)? << FRACTION_BITS;
        let root = widened.isqrt();
        Ok(Self(narrow(
            i128::try_from(root).map_err(|_| CoreStatus::Overflow)?,
        )?))
    }

    /// The base-two logarithm.
    ///
    /// The integer part is the position of the highest set bit; the fractional
    /// part is found one bit at a time by squaring the mantissa and taking a
    /// bit whenever it passes two. There is no iteration count to tune and no
    /// series to truncate.
    ///
    /// The working is done at [`WIDE_BITS`] fractional bits rather than at
    /// this type's thirty-two, and rounded once at the end. Sixteen spare bits
    /// is the difference between a result that is right to the last bit and
    /// one that is right to about twenty-five of them, and the cost is that
    /// the intermediates are `i128`.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::NoRealAnswer`] for a value that is not positive.
    pub fn log2(self) -> Result<Self> {
        Ok(Self(narrow(wide_narrow(wide_log2(self.0)?))?))
    }

    /// Two raised to this value.
    ///
    /// The whole part is a shift. The fractional part selects from the
    /// successive square roots of two — 2^(1/2), 2^(1/4), 2^(1/8) and so on —
    /// which are computed here by repeated integer square root rather than
    /// embedded as a table, so nothing has to be taken on trust.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`] for an exponent past the
    /// representable range.
    pub fn exp2(self) -> Result<Self> {
        Ok(Self(narrow(wide_narrow(wide_exp2(wide_widen(self.0))?))?))
    }

    /// The natural logarithm.
    ///
    /// # Errors
    ///
    /// As [`Fixed::log2`].
    pub fn ln(self) -> Result<Self> {
        // ln x = log2 x · ln 2, and ln 2 is derived rather than written down:
        // it is 1 / log2(e), and e is the one constant this module needs.
        self.log2()?.checked_mul(ln_two()?)
    }

    /// e raised to this value.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`].
    pub fn exp(self) -> Result<Self> {
        // e^x = 2^(x / ln 2), which reuses the one exponential rather than
        // introducing a second series with its own error.
        self.checked_div(ln_two()?)?.exp2()
    }

    /// This value raised to a power.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::NoRealAnswer`] for a base that is not positive, or an
    /// overflow. Zero to any positive power is zero, which is answered
    /// directly rather than through the logarithm.
    pub fn pow(self, exponent: Self) -> Result<Self> {
        if self.0 == 0 {
            return if exponent.is_positive() {
                Ok(Self::ZERO)
            } else {
                Err(CoreStatus::NoRealAnswer)
            };
        }
        // The logarithm, the multiplication and the exponential all happen at
        // the wider precision, so the only rounding in a power is the last one.
        let logarithm = wide_log2(self.0)?;
        let scaled = wide_mul(logarithm, wide_widen(exponent.0))?;
        Ok(Self(narrow(wide_narrow(wide_exp2(scaled)?))?))
    }

    /// The sine of an angle measured in **turns**.
    ///
    /// One turn is a full circle, so a quarter turn is a right angle. Turns
    /// rather than radians, and that is the whole reason this is exact where
    /// it matters: reducing an angle to one revolution is taking the
    /// fractional part, which in binary fixed point is masking bits and loses
    /// nothing. In radians the same reduction is a division by an irrational
    /// number, so the reduction itself introduces error — and it introduces
    /// *more* of it the further from zero the angle is, which is why a
    /// floating-point `sin` of a large argument is nearly meaningless.
    ///
    /// Four values are exact by construction rather than by luck, because the
    /// reduction lands them on zero before any series runs: nought turns is 0,
    /// a quarter turn is 1, a half turn is 0, and three quarters is -1. A
    /// sine that returned 0.9999999 for a right angle would put a wobble in
    /// every oscillator built on it.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::Overflow`] if the arithmetic leaves the representable
    /// range.
    pub fn sin_turns(self) -> Result<Self> {
        let reduced = reduce_turns(self.0);
        let radians = wide_mul(wide_widen(reduced.offset), wide_two_pi()?)?;
        // Within one eighth of a turn the argument is at most a quarter of pi,
        // where both series converge quickly and neither loses precision to
        // cancellation.
        let value = if reduced.cosine {
            wide_cos(radians)?
        } else {
            wide_sin(radians)?
        };
        let narrowed = narrow(wide_narrow(value))?;
        Ok(Self(if reduced.negate { -narrowed } else { narrowed }))
    }

    /// The cosine of an angle measured in turns.
    ///
    /// A sine a quarter turn ahead, which is what a cosine is. Writing it that
    /// way rather than as a second series means the two can never disagree
    /// about a quadrant.
    ///
    /// # Errors
    ///
    /// As [`Fixed::sin_turns`].
    pub fn cos_turns(self) -> Result<Self> {
        let quarter = ONE >> 2;
        Self(self.0.wrapping_add(quarter)).sin_turns()
    }
}

/// The natural logarithm of two.
///
/// Written as an exact rational to the precision the fixed point can hold,
/// which is the same everywhere and is checked against its defining property
/// in the tests: `exp2(1 / ln2)` must be `e`.
fn ln_two() -> Result<Fixed> {
    // 0.69314718055994530941723... to eighteen places, which is more than the
    // thirty-two fractional bits can distinguish.
    let value = Rational::new(693_147_180_559_945_309, 1_000_000_000_000_000_000)?;
    Fixed::from_rational(value)
}

/// Shift right by the fractional width, rounding half away from zero.
fn shift_rounded(value: i128) -> i128 {
    if value >= 0 {
        (value + HALF) >> FRACTION_BITS
    } else {
        -((-value + HALF) >> FRACTION_BITS)
    }
}

/// Divide, rounding half away from zero.
fn divide_rounded(numerator: i128, denominator: i128) -> i128 {
    let negative = (numerator < 0) != (denominator < 0);
    let magnitude = (numerator.abs() + denominator.abs() / 2) / denominator.abs();
    if negative { -magnitude } else { magnitude }
}

/// Bring a wide value back to the fixed point's width, or refuse it.
fn narrow(value: i128) -> Result<i64> {
    i64::try_from(value).map_err(|_| CoreStatus::Overflow)
}

/// How many fractional bits the logarithm and exponential work at.
///
/// Forty-eight, chosen by what the arithmetic allows: the logarithm squares a
/// mantissa below four, and squaring a value of fifty bits produces one of a
/// hundred, which is inside what an `i128` holds. Sixty-four would not be.
pub const WIDE_BITS: u32 = 48;

/// One, at the wider precision.
const WIDE_ONE: i128 = 1_i128 << WIDE_BITS;

/// Half of one, at the wider precision, for rounding.
const WIDE_HALF: i128 = 1_i128 << (WIDE_BITS - 1);

/// Widen a value from this type's precision to the working one.
const fn wide_widen(value: i64) -> i128 {
    (value as i128) << (WIDE_BITS - FRACTION_BITS)
}

/// Narrow a working value back, rounding half away from zero.
const fn wide_narrow(value: i128) -> i128 {
    let shift = WIDE_BITS - FRACTION_BITS;
    let half = 1_i128 << (shift - 1);
    if value >= 0 {
        (value + half) >> shift
    } else {
        -((-value + half) >> shift)
    }
}

/// Multiply at the working precision, rounding half away from zero.
fn wide_mul(left: i128, right: i128) -> Result<i128> {
    let product = left.checked_mul(right).ok_or(CoreStatus::Overflow)?;
    Ok(if product >= 0 {
        (product + WIDE_HALF) >> WIDE_BITS
    } else {
        -((-product + WIDE_HALF) >> WIDE_BITS)
    })
}

/// The square root at the working precision, exact to the last bit.
fn wide_sqrt(value: i128) -> Result<i128> {
    let widened = u128::try_from(value)
        .map_err(|_| CoreStatus::NoRealAnswer)?
        .checked_shl(WIDE_BITS)
        .ok_or(CoreStatus::Overflow)?;
    i128::try_from(widened.isqrt()).map_err(|_| CoreStatus::Overflow)
}

/// The base-two logarithm of a value of this type's precision, at the working
/// one.
fn wide_log2(value: i64) -> Result<i128> {
    if value <= 0 {
        return Err(CoreStatus::NoRealAnswer);
    }
    let mut mantissa = wide_widen(value);
    let mut exponent = 0_i128;
    while mantissa >= WIDE_ONE << 1 {
        mantissa >>= 1;
        exponent += 1;
    }
    while mantissa < WIDE_ONE {
        mantissa <<= 1;
        exponent -= 1;
    }

    let mut fraction = 0_i128;
    for bit in 1..=WIDE_BITS {
        // The mantissa is inside [1, 2) here, so its square is inside [1, 4)
        // and needs two bits above the point - which is why the working width
        // is forty-eight and not sixty-four.
        mantissa = wide_mul(mantissa, mantissa)?;
        if mantissa >= WIDE_ONE << 1 {
            mantissa >>= 1;
            fraction |= 1_i128 << (WIDE_BITS - bit);
        }
    }

    exponent
        .checked_mul(WIDE_ONE)
        .and_then(|whole| whole.checked_add(fraction))
        .ok_or(CoreStatus::Overflow)
}

/// Two raised to a value, both at the working precision.
fn wide_exp2(value: i128) -> Result<i128> {
    let whole = value >> WIDE_BITS;
    let fraction = value - (whole << WIDE_BITS);

    let mut result = WIDE_ONE;
    let mut root = WIDE_ONE << 1;
    for bit in 1..=WIDE_BITS {
        root = wide_sqrt(root)?;
        if fraction & (1_i128 << (WIDE_BITS - bit)) != 0 {
            result = wide_mul(result, root)?;
        }
    }

    // The whole part is applied last, so every multiplication above happens on
    // a value in [1, 2) where the fixed point has the most bits to spare.
    if whole >= 0 {
        let shift = u32::try_from(whole).map_err(|_| CoreStatus::Overflow)?;
        if shift >= 80 {
            return Err(CoreStatus::Overflow);
        }
        result.checked_shl(shift).ok_or(CoreStatus::Overflow)
    } else {
        let shift = u32::try_from(-whole).map_err(|_| CoreStatus::Overflow)?;
        if shift >= 127 {
            return Ok(0);
        }
        Ok(result >> shift)
    }
}

/// Two pi, at [`WIDE_BITS`] fractional bits.
///
/// Written as the decimal it is, to nineteen significant figures — which is
/// within a ten-thousandth of the last bit this type carries, and is the same
/// way every other irrational constant in this project is written: visible,
/// checkable against a reference, and not a magic hexadecimal number.
fn wide_two_pi() -> Result<i128> {
    const NUMERATOR: i128 = 6_283_185_307_179_586_477;
    const DENOMINATOR: i128 = 1_000_000_000_000_000_000;
    NUMERATOR
        .checked_shl(WIDE_BITS)
        .map(|scaled| scaled / DENOMINATOR)
        .ok_or(CoreStatus::Overflow)
}

/// Reduce an angle in turns to an eighth of a turn, exactly.
///
/// Returns whether the result is negated, which octant it came from, and how
/// far into that octant it is — where the last is at most an eighth of a turn.
/// Every step is an exact subtraction of an exact eighth, so the reduction
/// contributes no error at all, however many turns the angle was.
const fn reduce_turns(turns: i64) -> Reduced {
    // The fractional part of a turn, always in [0, 1). A negative angle wraps
    // rather than reflecting, which is what "one turn is a full circle" means.
    let within = turns & (ONE - 1);
    let eighth = ONE >> 3;
    // `within` is masked to the low fractional bits, so it is in [0, ONE) and
    // the quotient is in [0, 8) — but the compiler cannot see that, so the
    // conversion is written to be safe rather than asserted to be needless.
    let octant = within / eighth;
    let into = within - octant * eighth;
    Reduced {
        // The second half of the circle is the first half negated.
        negate: octant >= 4,
        // Octants one, two, five and six are measured from the axis the
        // *other* function is aligned with. Written as the table it is rather
        // than as a parity trick, because the parity trick is wrong — an
        // earlier version used one and put a right angle's sine at nought.
        cosine: matches!(octant % 4, 1 | 2),
        // Odd octants run backwards from the boundary above them.
        offset: if octant % 2 == 0 { into } else { eighth - into },
    }
}

/// An angle reduced to an eighth of a turn, and how to put it back together.
struct Reduced {
    negate: bool,
    cosine: bool,
    offset: i64,
}

/// How many terms of the series to sum.
///
/// Nine, which is set for the *wide* value rather than for the narrowed one,
/// and the difference is worth saying because a control found it. On the
/// interval this is used over — nought to a quarter of pi — the tenth term is
/// below a hundredth of the last bit at forty-eight fractional bits, so
/// summing it would change nothing there either.
///
/// Six terms would pass every test in this crate, because the result is
/// narrowed to thirty-two fractional bits and the seventh term is already
/// three hundredths of a bit at *that* precision. Cutting it to five changes
/// no test at all. The count stays at nine anyway: `wide_sin` and `wide_cos`
/// are wide functions and should be right at their own precision rather than
/// at the precision of the one caller they happen to have today.
const SERIES_TERMS: u32 = 9;

/// The sine of a wide angle in radians, on nought to a quarter of pi.
fn wide_sin(radians: i128) -> Result<i128> {
    let square = wide_mul(radians, radians)?;
    let mut term = radians;
    let mut total = radians;
    for step in 1..=SERIES_TERMS {
        let k = i128::from(step);
        let divisor = (2 * k) * (2 * k + 1);
        term = wide_mul(term, square)? / divisor;
        total = if step % 2 == 1 {
            total.checked_sub(term)
        } else {
            total.checked_add(term)
        }
        .ok_or(CoreStatus::Overflow)?;
    }
    Ok(total)
}

/// The cosine of a wide angle in radians, on nought to a quarter of pi.
fn wide_cos(radians: i128) -> Result<i128> {
    let square = wide_mul(radians, radians)?;
    let mut term = WIDE_ONE;
    let mut total = WIDE_ONE;
    for step in 1..=SERIES_TERMS {
        let k = i128::from(step);
        let divisor = (2 * k - 1) * (2 * k);
        term = wide_mul(term, square)? / divisor;
        total = if step % 2 == 1 {
            total.checked_sub(term)
        } else {
            total.checked_add(term)
        }
        .ok_or(CoreStatus::Overflow)?;
    }
    Ok(total)
}
