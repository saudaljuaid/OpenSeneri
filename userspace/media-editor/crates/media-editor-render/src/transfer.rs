// SPDX-License-Identifier: GPL-3.0-only
//! Transfer functions: the relationship between stored code values and light.
//!
//! Often called gamma. It is not a synonym — a gamma is one shape this can
//! take, and the two that matter most for high dynamic range are not gammas at
//! all.
//!
//! Every constant here is the exact decimal its standard prints, written as a
//! rational and converted once. Every computation is the fixed-point
//! arithmetic in [`media_editor_core::Fixed`], so the result is the same on every machine
//! (R-4.1) and needs no floating point, which is what lets it run on Phipia at
//! all.
//!
//! Both directions are here, and they are named for what they do rather than
//! for the acronym of the moment: [`encode`] takes light to code values, and
//! [`decode`] takes code values to light.

use media_editor_core::Rational;
use media_editor_media::TransferFunction;

use crate::status::{RenderStatus, Result};
use media_editor_core::Fixed;

/// An exact decimal, as a standard prints it.
fn constant(numerator: i64, denominator: i64) -> media_editor_core::Result<Fixed> {
    Fixed::from_rational(Rational::new(numerator, denominator)?)
}

/// Take light to code values.
///
/// # Errors
///
/// [`RenderStatus::OutsideDomain`] for a negative value. Negative light is not
/// something these standards define, and a pipeline that clamped it silently
/// would hide a bug upstream rather than report one (R-1.3).
pub fn encode(transfer: TransferFunction, linear: Fixed) -> Result<Fixed> {
    if linear.raw() < 0 {
        return Err(RenderStatus::OutsideDomain);
    }
    Ok(match transfer {
        TransferFunction::Linear => Ok(linear),
        TransferFunction::Gamma22 => linear.pow(constant(10, 22)?),
        TransferFunction::Gamma26 => linear.pow(constant(10, 26)?),
        TransferFunction::Srgb => {
            // IEC 61966-2-1: below the breakpoint the curve is a straight
            // line, which keeps the slope finite at zero.
            if linear <= constant(31_308, 10_000_000)? {
                linear.checked_mul(constant(1292, 100)?)
            } else {
                constant(1055, 1000)?
                    .checked_mul(linear.pow(constant(10, 24)?)?)?
                    .checked_sub(constant(55, 1000)?)
            }
        }
        TransferFunction::Bt709 | TransferFunction::Bt2020Ten => {
            // ITU-R BT.709-6 and BT.2020-2 at ten bits share these constants.
            if linear < constant(18, 1000)? {
                linear.checked_mul(constant(45, 10)?)
            } else {
                constant(1099, 1000)?
                    .checked_mul(linear.pow(constant(45, 100)?)?)?
                    .checked_sub(constant(99, 1000)?)
            }
        }
        TransferFunction::PerceptualQuantiser => {
            // SMPTE ST 2084. The input is luminance normalised so that one is
            // ten thousand candelas per square metre.
            let powered = linear.pow(pq_m1()?)?;
            let numerator = pq_c1()?.checked_add(pq_c2()?.checked_mul(powered)?)?;
            let denominator = Fixed::ONE.checked_add(pq_c3()?.checked_mul(powered)?)?;
            numerator.checked_div(denominator)?.pow(pq_m2()?)
        }
        TransferFunction::HybridLogGamma => {
            // ITU-R BT.2100 table 5. Below a twelfth the curve is a square
            // root, which is what makes it compatible with older displays.
            if linear <= constant(1, 12)? {
                linear.checked_mul(Fixed::from_integer(3)?)?.sqrt()
            } else {
                let inner = linear
                    .checked_mul(Fixed::from_integer(12)?)?
                    .checked_sub(hlg_b()?)?;
                hlg_a()?.checked_mul(inner.ln()?)?.checked_add(hlg_c()?)
            }
        }
    }?)
}

/// Take code values to light.
///
/// # Errors
///
/// [`RenderStatus::OutsideDomain`] for a negative value, and
/// [`RenderStatus::Singular`] where a standard's inverse is undefined — which
/// for the perceptual quantiser is any code value at or above one, where its
/// denominator reaches zero.
pub fn decode(transfer: TransferFunction, encoded: Fixed) -> Result<Fixed> {
    if encoded.raw() < 0 {
        return Err(RenderStatus::OutsideDomain);
    }
    Ok(match transfer {
        TransferFunction::Linear => Ok(encoded),
        TransferFunction::Gamma22 => encoded.pow(constant(22, 10)?),
        TransferFunction::Gamma26 => encoded.pow(constant(26, 10)?),
        TransferFunction::Srgb => {
            if encoded <= constant(4045, 100_000)? {
                encoded.checked_div(constant(1292, 100)?)
            } else {
                encoded
                    .checked_add(constant(55, 1000)?)?
                    .checked_div(constant(1055, 1000)?)?
                    .pow(constant(24, 10)?)
            }
        }
        TransferFunction::Bt709 | TransferFunction::Bt2020Ten => {
            // The breakpoint of the inverse is the encoded value of the
            // forward one: 4.5 × 0.018.
            if encoded < constant(81, 1000)? {
                encoded.checked_div(constant(45, 10)?)
            } else {
                encoded
                    .checked_add(constant(99, 1000)?)?
                    .checked_div(constant(1099, 1000)?)?
                    .pow(constant(100, 45)?)
            }
        }
        TransferFunction::PerceptualQuantiser => {
            let powered = encoded.pow(Fixed::ONE.checked_div(pq_m2()?)?)?;
            let numerator = powered.checked_sub(pq_c1()?)?.max(Fixed::ZERO);
            let denominator = pq_c2()?.checked_sub(pq_c3()?.checked_mul(powered)?)?;
            if !denominator.is_positive() {
                return Err(RenderStatus::Singular);
            }
            numerator
                .checked_div(denominator)?
                .pow(Fixed::ONE.checked_div(pq_m1()?)?)
        }
        TransferFunction::HybridLogGamma => {
            if encoded <= constant(1, 2)? {
                encoded
                    .checked_mul(encoded)?
                    .checked_div(Fixed::from_integer(3)?)
            } else {
                encoded
                    .checked_sub(hlg_c()?)?
                    .checked_div(hlg_a()?)?
                    .exp()?
                    .checked_add(hlg_b()?)?
                    .checked_div(Fixed::from_integer(12)?)
            }
        }
    }?)
}

/// ST 2084's `m1`: 2610 / 16384.
fn pq_m1() -> media_editor_core::Result<Fixed> {
    constant(2610, 16_384)
}

/// ST 2084's `m2`: 2523 / 4096 × 128.
fn pq_m2() -> media_editor_core::Result<Fixed> {
    constant(2523 * 128, 4096)
}

/// ST 2084's `c1`: 3424 / 4096.
fn pq_c1() -> media_editor_core::Result<Fixed> {
    constant(3424, 4096)
}

/// ST 2084's `c2`: 2413 / 4096 × 32.
fn pq_c2() -> media_editor_core::Result<Fixed> {
    constant(2413 * 32, 4096)
}

/// ST 2084's `c3`: 2392 / 4096 × 32.
fn pq_c3() -> media_editor_core::Result<Fixed> {
    constant(2392 * 32, 4096)
}

/// BT.2100's hybrid log-gamma `a`.
fn hlg_a() -> media_editor_core::Result<Fixed> {
    constant(17_883_277, 100_000_000)
}

/// BT.2100's hybrid log-gamma `b`.
fn hlg_b() -> media_editor_core::Result<Fixed> {
    constant(28_466_892, 100_000_000)
}

/// BT.2100's hybrid log-gamma `c`.
fn hlg_c() -> media_editor_core::Result<Fixed> {
    constant(55_991_073, 100_000_000)
}
