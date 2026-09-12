// SPDX-License-Identifier: GPL-3.0-only
//! The vectorscope: hue and saturation, counted.
//!
//! A waveform answers "how bright"; a vectorscope answers "what colour, and how
//! much of it". It plots each sample's chroma as a point — blue-difference
//! across, red-difference up — so hue becomes an angle and saturation becomes a
//! distance from the middle. It is the instrument a colourist uses to say a
//! shot is warmer than the one before it, and to see that the neutrals are
//! neutral.
//!
//! Two things about it are exact, and both are properties rather than pinned
//! numbers.
//!
//! **Neutral is the origin.** Any sample with equal red, green and blue has
//! zero chroma in both axes, at every brightness and under every matrix. That
//! is what makes the middle of the graticule mean "no colour" — and it is
//! derived here, not asserted, so a matrix that got it wrong could not hide.
//!
//! **The primaries land on the axes.** Full red has a red-difference of
//! exactly one half, cyan exactly minus one half, full blue a blue-difference
//! of exactly one half and yellow exactly minus one half — in BT.601, BT.709
//! and BT.2020 alike, because `Cr = (R' - Y')/2(1 - Kr)` and full red makes
//! `Y' = Kr`, so the coefficient cancels itself out. Those four points are the
//! graticule's fixed marks and they are the same on every vectorscope ever
//! built. The other two corners — green and magenta — depend on the matrix,
//! which is exactly why a colourist can tell BT.601 material from BT.709 by
//! looking at where the green box lands.
//!
//! Chroma is computed from **gamma-encoded** samples, not from linear light.
//! That is not an oversight: `Y'CbCr` is defined on the encoded signal, the
//! primes in its name are the whole point, and a scope that decoded first
//! would put every box in the wrong place.

use alloc::vec::Vec;

use media_editor_core::Rational;
use media_editor_media::{ColourDescription, Frame, PixelFormat};

use crate::convert::{normalise, rgb_to_ycbcr};
use crate::status::{RenderStatus, Result};
use media_editor_core::{FRACTION_BITS, Fixed};

/// The most bins a vectorscope may have along one axis.
///
/// Two hundred and fifty-six, which is one bin per code value — finer than
/// that measures the quantisation rather than the picture. The square of it
/// bounds the allocation (R-11.2).
pub const MAX_BINS: usize = 256;

/// A two-dimensional count of a frame's chroma.
///
/// Bins are square and cover the whole chroma range, minus one half to plus one
/// half on each axis, so the middle of the grid is neutral. Counts are exact
/// integers: a vectorscope is a measurement, and two runs over one frame give
/// the same one.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Vectorscope {
    bins: usize,
    counts: Vec<u64>,
    total: u64,
}

impl Vectorscope {
    /// How many bins there are along each axis.
    #[must_use]
    pub const fn bins(&self) -> usize {
        self.bins
    }

    /// How many chroma samples were counted.
    #[must_use]
    pub const fn total(&self) -> u64 {
        self.total
    }

    /// How many samples fell in one bin.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::OutsideDomain`] if either index is past the grid.
    pub fn count(&self, blue_difference: usize, red_difference: usize) -> Result<u64> {
        if blue_difference >= self.bins || red_difference >= self.bins {
            return Err(RenderStatus::OutsideDomain);
        }
        self.counts
            .get(red_difference * self.bins + blue_difference)
            .copied()
            .ok_or(RenderStatus::OutsideDomain)
    }

    /// How many bins hold at least one sample.
    ///
    /// A flat colour occupies one; colour bars occupy as many as they have
    /// distinct chromas, which is fewer than they have bars.
    #[must_use]
    pub fn occupied(&self) -> usize {
        self.counts.iter().filter(|count| **count > 0).count()
    }

    /// Which bin a chroma value falls in.
    ///
    /// The value runs from minus one half to plus one half. The extremes are
    /// inclusive at both ends — a sample sitting exactly on plus one half
    /// belongs to the last bin rather than to one past it.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::OutsideDomain`] for a value outside that range.
    pub fn bin_of(&self, value: Rational) -> Result<usize> {
        let half = Rational::new(1, 2).map_err(RenderStatus::Time)?;
        let count = i64::try_from(self.bins).map_err(|_| RenderStatus::OutsideDomain)?;
        let shifted = value
            .checked_add(half)
            .map_err(RenderStatus::Time)?
            .checked_mul(Rational::new(count, 1).map_err(RenderStatus::Time)?)
            .map_err(RenderStatus::Time)?;
        if shifted.numerator() < 0 {
            return Err(RenderStatus::OutsideDomain);
        }
        let index = shifted.numerator() / shifted.denominator();
        let index = usize::try_from(index).map_err(|_| RenderStatus::OutsideDomain)?;
        if index > self.bins {
            return Err(RenderStatus::OutsideDomain);
        }
        Ok(index.min(self.bins - 1))
    }

    /// The bin neutral sits in: the middle of the grid.
    #[must_use]
    pub const fn neutral(&self) -> usize {
        self.bins / 2
    }
}

/// The exact chroma of one gamma-encoded colour, as a fraction of full scale.
///
/// Both values run from minus one half to plus one half. This is exact
/// rational arithmetic all the way through — the matrix is derived from the
/// primaries rather than looked up, so the answer is the real one and the same
/// on every machine (R-4.1).
///
/// # Errors
///
/// [`RenderStatus::Time`] wrapping an arithmetic refusal,
/// [`RenderStatus::DegenerateGamut`], or [`RenderStatus::Singular`].
pub fn chroma_of(
    colour: ColourDescription,
    red: Rational,
    green: Rational,
    blue: Rational,
) -> Result<(Rational, Rational)> {
    let matrix = rgb_to_ycbcr(colour)?;
    let rows = matrix.rows();
    let combine = |row: &[Rational; 3]| -> Result<Rational> {
        let mut sum = Rational::ZERO;
        for (coefficient, sample) in row.iter().zip([red, green, blue].iter()) {
            sum = sum
                .checked_add(
                    coefficient
                        .checked_mul(*sample)
                        .map_err(RenderStatus::Time)?,
                )
                .map_err(RenderStatus::Time)?;
        }
        Ok(sum)
    };
    Ok((combine(&rows[1])?, combine(&rows[2])?))
}

/// Count a frame's chroma into a square grid.
///
/// A luma-chroma frame is read from its own chroma planes, one point per chroma
/// sample — which is what a subsampled frame actually holds, so a 4:2:0 frame
/// contributes a quarter as many points as it has pixels rather than four
/// copies of each. An RGB frame is matrixed into `Y'CbCr` first, through the
/// matrix its primaries derive.
///
/// A single-channel frame has no chroma at all. That is not an error and it is
/// not nothing: a monochrome frame is genuinely at zero saturation, so every
/// sample lands on the origin, and a scope that refused to say so would be
/// hiding a true answer.
///
/// # Errors
///
/// [`RenderStatus::OutsideDomain`] for a bin count of zero or past
/// [`MAX_BINS`], [`RenderStatus::OutOfMemory`] if the grid cannot be held, and
/// any refusal from the colour arithmetic.
pub fn vectorscope(frame: &Frame, bins: usize) -> Result<Vectorscope> {
    if bins == 0 || bins > MAX_BINS {
        return Err(RenderStatus::OutsideDomain);
    }
    let cells = bins.checked_mul(bins).ok_or(RenderStatus::OutsideDomain)?;
    let mut counts = Vec::new();
    counts
        .try_reserve(cells)
        .map_err(|_| RenderStatus::OutOfMemory)?;
    counts.resize(cells, 0_u64);

    let mut scope = Vectorscope {
        bins,
        counts,
        total: 0,
    };
    let described = *frame.description();
    let colour = described.colour();

    if described.format().is_rgb() {
        let matrix = rgb_to_ycbcr(colour)?;
        let rows = matrix.rows();
        let blue_row = fixed_row(&rows[1])?;
        let red_row = fixed_row(&rows[2])?;
        let width = components(described.format());
        let packed = frame.packed()?;
        for pixel in packed.chunks_exact(width) {
            let sample = if width == 1 {
                // One channel is a grey: equal red, green and blue, which is
                // the origin whatever the matrix says.
                let value = normalise(pixel[0], colour.range, false)?;
                [value, value, value]
            } else {
                [
                    normalise(pixel[0], colour.range, false)?,
                    normalise(pixel[1], colour.range, false)?,
                    normalise(pixel[2], colour.range, false)?,
                ]
            };
            let blue_difference = weighted(&blue_row, sample)?;
            let red_difference = weighted(&red_row, sample)?;
            scope.record(blue_difference, red_difference)?;
        }
        return Ok(scope);
    }

    // Planar: the chroma planes already hold what a vectorscope plots.
    let blue_plane = frame.plane(1)?;
    let red_plane = frame.plane(2)?;
    let plane_geometry = described.format().plane_geometry(described.geometry(), 1)?;
    let rows = usize::try_from(plane_geometry.height()).map_err(|_| RenderStatus::OutsideDomain)?;
    let columns =
        usize::try_from(plane_geometry.width()).map_err(|_| RenderStatus::OutsideDomain)?;
    for row in 0..rows {
        let blue_row = blue_plane.row(row)?;
        let red_row = red_plane.row(row)?;
        for column in 0..columns {
            let blue = *blue_row.get(column).ok_or(RenderStatus::OutsideDomain)?;
            let red = *red_row.get(column).ok_or(RenderStatus::OutsideDomain)?;
            scope.record(
                normalise(blue, colour.range, true)?,
                normalise(red, colour.range, true)?,
            )?;
        }
    }
    Ok(scope)
}

impl Vectorscope {
    /// Put one chroma point in its bin.
    fn record(&mut self, blue_difference: Fixed, red_difference: Fixed) -> Result<()> {
        let blue = bin_of_fixed(blue_difference, self.bins)?;
        let red = bin_of_fixed(red_difference, self.bins)?;
        let cell = red
            .checked_mul(self.bins)
            .and_then(|value| value.checked_add(blue))
            .ok_or(RenderStatus::OutsideDomain)?;
        let slot = self
            .counts
            .get_mut(cell)
            .ok_or(RenderStatus::OutsideDomain)?;
        *slot = slot.checked_add(1).ok_or(RenderStatus::OutsideDomain)?;
        self.total = self
            .total
            .checked_add(1)
            .ok_or(RenderStatus::OutsideDomain)?;
        Ok(())
    }
}

/// Which bin a fixed-point chroma value falls in, clamped to the grid.
///
/// Clamping rather than refusing, because a limited-range frame may legally
/// hold a chroma sample past the nominal range — the space between the legal
/// and the illegal code values exists precisely so that a filter's overshoot
/// has somewhere to go. A scope that refused to plot it would be hiding the
/// one sample a colourist most wants to see.
fn bin_of_fixed(value: Fixed, bins: usize) -> Result<usize> {
    let half = Fixed::ONE.raw() / 2;
    let shifted = value.raw().saturating_add(half);
    if shifted < 0 {
        return Ok(0);
    }
    let count = i64::try_from(bins).map_err(|_| RenderStatus::OutsideDomain)?;
    let scaled = shifted
        .checked_mul(count)
        .ok_or(RenderStatus::OutsideDomain)?
        >> FRACTION_BITS;
    let index = usize::try_from(scaled).map_err(|_| RenderStatus::OutsideDomain)?;
    Ok(index.min(bins - 1))
}

/// One row of a derived matrix, ready for per-sample arithmetic.
fn fixed_row(row: &[Rational; 3]) -> Result<[Fixed; 3]> {
    Ok([
        Fixed::from_rational(row[0])?,
        Fixed::from_rational(row[1])?,
        Fixed::from_rational(row[2])?,
    ])
}

/// The weighted sum of three samples.
fn weighted(row: &[Fixed; 3], sample: [Fixed; 3]) -> Result<Fixed> {
    let mut sum = Fixed::ZERO;
    for (coefficient, value) in row.iter().zip(sample.iter()) {
        sum = sum.checked_add(coefficient.checked_mul(*value)?)?;
    }
    Ok(sum)
}

/// How many components a packed format interleaves.
const fn components(format: PixelFormat) -> usize {
    match format {
        PixelFormat::Rgba8 => 4,
        PixelFormat::Rgb8 => 3,
        _ => 1,
    }
}
