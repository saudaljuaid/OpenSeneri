// SPDX-License-Identifier: GPL-3.0-only
//! Where a set of primaries actually is.
//!
//! A standard defines a gamut by four pairs of exact decimal numbers: the
//! chromaticities of red, green and blue, and of the white point. Everything
//! else — the RGB to XYZ matrix, the luma coefficients, the matrix that takes
//! BT.709 to BT.2020 — is *derived* from those twelve numbers by linear
//! algebra.
//!
//! Almost every implementation does that derivation in floating point once,
//! rounds the result to four decimal places, and publishes the rounding. This
//! one does it in exact rationals every time, so the matrix is the real one
//! and is identical on every machine (R-4.1).
//!
//! A worked consequence, which the tests check: BT.709's luma coefficients
//! derived from its own primaries round to 0.2126, 0.7152 and 0.0722 — the
//! numbers the standard prints. BT.601's do **not** round to 0.299, 0.587 and
//! 0.114, because those coefficients came from the 1953 NTSC primaries and
//! were kept when the primaries changed. Matrix coefficients and primaries are
//! different things, and treating them as one is a classic way to get colour
//! wrong.

use media_editor_core::Rational;
use media_editor_media::Primaries;

use crate::matrix::{Matrix3, Vector3};
use crate::status::{RenderStatus, Result};

/// A point in the CIE xy chromaticity diagram.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Chromaticity {
    x: Rational,
    y: Rational,
}

impl Chromaticity {
    /// A chromaticity.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::DegenerateChromaticity`] if `y` is zero, because the
    /// conversion to tristimulus values divides by it.
    pub fn new(x: Rational, y: Rational) -> Result<Self> {
        if y.is_zero() {
            return Err(RenderStatus::DegenerateChromaticity);
        }
        Ok(Self { x, y })
    }

    /// A chromaticity from an exact decimal, as a standard prints it.
    ///
    /// `Chromaticity::decimal(640, 330, 1000)` is (0.640, 0.330).
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Time`] for a zero or unrepresentable denominator, or
    /// [`RenderStatus::DegenerateChromaticity`].
    pub fn decimal(x: i64, y: i64, scale: i64) -> Result<Self> {
        Self::new(Rational::new(x, scale)?, Rational::new(y, scale)?)
    }

    /// The x coordinate.
    #[must_use]
    pub const fn x(self) -> Rational {
        self.x
    }

    /// The y coordinate.
    #[must_use]
    pub const fn y(self) -> Rational {
        self.y
    }

    /// This chromaticity as tristimulus values at unit luminance.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Time`] wrapping an arithmetic refusal.
    pub fn tristimulus(self) -> Result<Vector3> {
        let big_x = self.x.checked_div(self.y)?;
        let z = Rational::ONE
            .checked_sub(self.x)?
            .checked_sub(self.y)?
            .checked_div(self.y)?;
        Ok(Vector3::new(big_x, Rational::ONE, z))
    }
}

/// The four chromaticities that define a gamut.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Gamut {
    /// Where red is.
    pub red: Chromaticity,
    /// Where green is.
    pub green: Chromaticity,
    /// Where blue is.
    pub blue: Chromaticity,
    /// What counts as white.
    pub white: Chromaticity,
}

impl Gamut {
    /// The primary tristimulus matrix and the scale factors, which is the
    /// gamut in the form everything else is derived from.
    ///
    /// `rgb_to_xyz` is the product of these two, and `xyz_to_rgb` is the
    /// product of their inverses in the other order. Keeping them apart is
    /// what makes the derivation exact in practice as well as in principle:
    /// inverting the product squares its denominators, and inverting the
    /// factors does not.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::DegenerateGamut`] if the three primaries are collinear,
    /// so that no scaling of them reaches the white point.
    fn factors(&self) -> Result<(Matrix3, Vector3)> {
        let red = self.red.tristimulus()?;
        let green = self.green.tristimulus()?;
        let blue = self.blue.tristimulus()?;

        // Columns are the primaries, so a row of this matrix is one
        // tristimulus component across red, green and blue.
        let primaries = Matrix3::from_rows([
            [red.elements()[0], green.elements()[0], blue.elements()[0]],
            [red.elements()[1], green.elements()[1], blue.elements()[1]],
            [red.elements()[2], green.elements()[2], blue.elements()[2]],
        ]);

        let white = self.white.tristimulus()?;
        let scale = primaries
            .inverse()
            .map_err(|status| match status {
                RenderStatus::Singular => RenderStatus::DegenerateGamut,
                other => other,
            })?
            .apply(white)?;
        Ok((primaries, scale))
    }

    /// The matrix that takes linear RGB in this gamut to CIE XYZ.
    ///
    /// The derivation is the standard one and is exact at every step: build
    /// the matrix of primary tristimulus values, solve it against the white
    /// point to find how much of each primary white is made of, and scale the
    /// columns by that.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::DegenerateGamut`] if the three primaries are collinear.
    pub fn rgb_to_xyz(&self) -> Result<Matrix3> {
        let (primaries, scale) = self.factors()?;
        primaries.multiply(&Matrix3::diagonal(scale))
    }

    /// The matrix that takes CIE XYZ to linear RGB in this gamut.
    ///
    /// # Errors
    ///
    /// As [`Gamut::rgb_to_xyz`], and [`RenderStatus::DegenerateGamut`] if any
    /// primary contributes nothing to white, which would make the diagonal
    /// singular.
    pub fn xyz_to_rgb(&self) -> Result<Matrix3> {
        let (primaries, scale) = self.factors()?;
        let mut reciprocals = [Rational::ZERO; 3];
        for (slot, value) in reciprocals.iter_mut().zip(scale.elements()) {
            *slot = value
                .checked_reciprocal()
                .map_err(|_| RenderStatus::DegenerateGamut)?;
        }
        let inverse_scale =
            Matrix3::diagonal(Vector3::new(reciprocals[0], reciprocals[1], reciprocals[2]));
        inverse_scale.multiply(&primaries.inverse()?)
    }

    /// How much each primary contributes to luminance, derived from the
    /// primaries themselves.
    ///
    /// This is the Y row of [`Gamut::rgb_to_xyz`]. Compare it with the luma
    /// coefficients a standard specifies for its matrix: for BT.709 and
    /// BT.2020 they agree, and for BT.601 they do not.
    ///
    /// # Errors
    ///
    /// As [`Gamut::rgb_to_xyz`].
    pub fn derived_luminance(&self) -> Result<Vector3> {
        // The Y row of the primary matrix is all ones, because a tristimulus
        // value at unit luminance has Y = 1 by construction. So the Y row of
        // the product is the scale factors themselves.
        let (_, scale) = self.factors()?;
        Ok(scale)
    }

    /// The matrix that takes linear RGB in this gamut to linear RGB in
    /// another, with no adaptation of the white point.
    ///
    /// White-point adaptation is a separate decision with several defensible
    /// answers, so it is a separate operation rather than something this does
    /// quietly. Between two gamuts that share a white point — which every pair
    /// in [`Primaries`] does except the DCI-P3 one — there is nothing to
    /// adapt.
    ///
    /// # Errors
    ///
    /// As [`Gamut::rgb_to_xyz`].
    pub fn to_gamut(&self, target: &Self) -> Result<Matrix3> {
        target.xyz_to_rgb()?.multiply(&self.rgb_to_xyz()?)
    }
}

/// The gamut a set of primaries names.
///
/// Every number here is the exact decimal its standard prints. They are not
/// rounded, adjusted, or taken from another implementation.
///
/// # Errors
///
/// [`RenderStatus::Time`] never happens for these constants; the result exists
/// because the constructors that build them are fallible.
pub fn gamut_of(primaries: Primaries) -> Result<Gamut> {
    // D65 as the video standards state it: x = 0.3127, y = 0.3290.
    let d65 = Chromaticity::decimal(3127, 3290, 10_000)?;
    // DCI's white, which is not D65 and is a different colour.
    let dci = Chromaticity::decimal(314, 351, 1000)?;
    // ACES white, near D60.
    let aces_white = Chromaticity::decimal(32_168, 33_767, 100_000)?;

    Ok(match primaries {
        // ITU-R BT.709-6, table 1.
        Primaries::Bt709 => Gamut {
            red: Chromaticity::decimal(640, 330, 1000)?,
            green: Chromaticity::decimal(300, 600, 1000)?,
            blue: Chromaticity::decimal(150, 60, 1000)?,
            white: d65,
        },
        // SMPTE 170M, which is what BT.601 means in 525-line territories.
        Primaries::Bt601Ntsc => Gamut {
            red: Chromaticity::decimal(630, 340, 1000)?,
            green: Chromaticity::decimal(310, 595, 1000)?,
            blue: Chromaticity::decimal(155, 70, 1000)?,
            white: d65,
        },
        // ITU-R BT.470-6 System B/G, the 625-line primaries.
        Primaries::Bt601Pal => Gamut {
            red: Chromaticity::decimal(640, 330, 1000)?,
            green: Chromaticity::decimal(290, 600, 1000)?,
            blue: Chromaticity::decimal(150, 60, 1000)?,
            white: d65,
        },
        // ITU-R BT.2020-2, table 2.
        Primaries::Bt2020 => Gamut {
            red: Chromaticity::decimal(708, 292, 1000)?,
            green: Chromaticity::decimal(170, 797, 1000)?,
            blue: Chromaticity::decimal(131, 46, 1000)?,
            white: d65,
        },
        // SMPTE RP 431-2, the digital cinema projector, with DCI white.
        Primaries::DciP3 => Gamut {
            red: Chromaticity::decimal(680, 320, 1000)?,
            green: Chromaticity::decimal(265, 690, 1000)?,
            blue: Chromaticity::decimal(150, 60, 1000)?,
            white: dci,
        },
        // The same primaries at D65, which is what a display means by P3.
        Primaries::DisplayP3 => Gamut {
            red: Chromaticity::decimal(680, 320, 1000)?,
            green: Chromaticity::decimal(265, 690, 1000)?,
            blue: Chromaticity::decimal(150, 60, 1000)?,
            white: d65,
        },
        // Academy S-2014-003, AP0: the encompassing gamut.
        Primaries::AcesAp0 => Gamut {
            red: Chromaticity::decimal(7347, 2653, 10_000)?,
            green: Chromaticity::new(Rational::ZERO, Rational::ONE)?,
            blue: Chromaticity::decimal(1, -770, 10_000)?,
            white: aces_white,
        },
        // Academy S-2014-004, AP1: the working gamut.
        Primaries::AcesAp1 => Gamut {
            red: Chromaticity::decimal(713, 293, 1000)?,
            green: Chromaticity::decimal(165, 830, 1000)?,
            blue: Chromaticity::decimal(128, 44, 1000)?,
            white: aces_white,
        },
    })
}
