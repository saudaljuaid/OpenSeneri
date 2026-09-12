// SPDX-License-Identifier: GPL-3.0-only
//! Three-dimensional colour lookup tables.
//!
//! A table stores `size³` colour samples and interpolates between them. Both
//! trilinear and tetrahedral interpolation are available. Tetrahedral
//! interpolation follows four vertices and preserves a neutral table's grey
//! axis exactly. Fixed-point arithmetic is sufficient for the sampled data and
//! keeps lattice points unchanged.

use alloc::vec::Vec;

use media_editor_core::Fixed;

use crate::status::{RenderStatus, Result};

/// The fewest samples a side a table may have.
///
/// Two is a cube with only its corners, which is an affine transform written
/// the long way. It is legal, and it is the smallest thing that can be
/// interpolated at all.
pub const MIN_SIZE: usize = 2;

/// The most samples a side a table may have.
///
/// Sixty-five, which is 274,625 samples and the largest size any format in
/// common use writes. The bound exists so that nothing can ask for a cube the
/// size of memory before anything checks (R-11.2).
pub const MAX_SIZE: usize = 65;

/// A colour, as three components of light or code value.
///
/// Whatever space the table was authored for. A table does not know or care —
/// it maps three numbers to three numbers — and choosing the space is the
/// caller's decision to make and to name (R-1.3).
pub type Colour = [Fixed; 3];

/// How to interpolate between a table's samples.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Interpolation {
    /// Four vertices of the tetrahedron containing the sample.
    ///
    /// What every professional application uses, and what this project
    /// defaults to, because it keeps the neutral axis neutral.
    Tetrahedral,
    /// All eight corners of the surrounding cell, weighted by position.
    ///
    /// Here to be measured against, and to be available for anyone who needs
    /// to reproduce another system's output bit for bit — which is a real
    /// requirement and a different one from wanting the better answer.
    Trilinear,
}

/// A cube of sampled colours.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Lut3D {
    /// Samples a side.
    size: usize,
    /// `size³` colours, red varying fastest.
    ///
    /// Red fastest because that is the order every interchange format writes
    /// them in, and an index arithmetic that disagreed with the file would
    /// transpose the cube — which looks like a plausible grade rather than
    /// like a bug.
    samples: Vec<Colour>,
}

impl Lut3D {
    /// Wrap a cube of samples.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::LutSizeUnsupported`] outside [`MIN_SIZE`] to
    /// [`MAX_SIZE`], and [`RenderStatus::LutNotACube`] if the sample count is
    /// not the cube of the size — which is the one relationship a file can get
    /// wrong in a way that still parses.
    pub fn new(size: usize, samples: Vec<Colour>) -> Result<Self> {
        if !(MIN_SIZE..=MAX_SIZE).contains(&size) {
            return Err(RenderStatus::LutSizeUnsupported);
        }
        let wanted = size
            .checked_mul(size)
            .and_then(|square| square.checked_mul(size))
            .ok_or(RenderStatus::LutNotACube)?;
        if samples.len() != wanted {
            return Err(RenderStatus::LutNotACube);
        }
        Ok(Self { size, samples })
    }

    /// A table that changes nothing.
    ///
    /// Useful on its own — a grade slot with nothing in it — and useful as the
    /// thing every other table is compared against.
    ///
    /// # Errors
    ///
    /// As [`Lut3D::new`], and [`RenderStatus::OutOfMemory`].
    pub fn identity(size: usize) -> Result<Self> {
        if !(MIN_SIZE..=MAX_SIZE).contains(&size) {
            return Err(RenderStatus::LutSizeUnsupported);
        }
        let mut samples = Vec::new();
        samples
            .try_reserve(size * size * size)
            .map_err(|_| RenderStatus::OutOfMemory)?;
        for blue in 0..size {
            for green in 0..size {
                for red in 0..size {
                    samples.push([
                        lattice(red, size)?,
                        lattice(green, size)?,
                        lattice(blue, size)?,
                    ]);
                }
            }
        }
        Self::new(size, samples)
    }

    /// Samples a side.
    #[must_use]
    pub const fn size(&self) -> usize {
        self.size
    }

    /// The sample at a lattice point.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::LutIndexOutOfRange`].
    pub fn sample(&self, red: usize, green: usize, blue: usize) -> Result<Colour> {
        if red >= self.size || green >= self.size || blue >= self.size {
            return Err(RenderStatus::LutIndexOutOfRange);
        }
        let index = red + self.size * (green + self.size * blue);
        self.samples
            .get(index)
            .copied()
            .ok_or(RenderStatus::LutIndexOutOfRange)
    }

    /// Whether every sample on the cube's own diagonal is neutral.
    ///
    /// The property that makes "a grey stays grey" mean anything: tetrahedral
    /// interpolation carries neutrality from the diagonal to every neutral
    /// input, so a table that is *not* neutral on its diagonal has no
    /// neutrality to carry. A look that deliberately tints its greys is a
    /// legitimate look, and this is how a caller can tell which kind it holds.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::LutIndexOutOfRange`], which cannot happen for a table
    /// this type built.
    pub fn is_neutral(&self) -> Result<bool> {
        for step in 0..self.size {
            let [red, green, blue] = self.sample(step, step, step)?;
            if red != green || green != blue {
                return Ok(false);
            }
        }
        Ok(true)
    }

    /// Look a colour up.
    ///
    /// Input components outside nought to one are clamped, because a table
    /// only holds what it holds: there is no sample beyond its edge and
    /// extrapolating from the last cell would invent a look nobody authored.
    /// That is a saturation at a real boundary rather than a decision hidden
    /// from the caller — the same distinction as a track opacity past full.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Time`] wrapping an arithmetic refusal.
    pub fn look_up(&self, colour: Colour, how: Interpolation) -> Result<Colour> {
        let mut cell = [0_usize; 3];
        let mut fraction = [Fixed::ZERO; 3];
        for channel in 0..3 {
            let (index, part) = self.place(colour[channel])?;
            cell[channel] = index;
            fraction[channel] = part;
        }
        match how {
            Interpolation::Tetrahedral => self.tetrahedral(cell, fraction),
            Interpolation::Trilinear => self.trilinear(cell, fraction),
        }
    }

    /// Which cell a component falls in, and how far through it.
    ///
    /// The top of the range lands in the *last* cell at a fraction of one
    /// rather than in a cell past the end at nought. Both name the same point,
    /// and this way the arithmetic below telescopes to the far corner exactly
    /// instead of reading past the cube.
    fn place(&self, component: Fixed) -> Result<(usize, Fixed)> {
        let clamped = component.clamp(Fixed::ZERO, Fixed::ONE);
        let steps = i64::try_from(self.size - 1).map_err(|_| overflow())?;
        let scaled = clamped.checked_mul(Fixed::from_integer(steps)?)?;
        let whole = scaled.raw() >> media_editor_core::FRACTION_BITS;
        let index = usize::try_from(whole).map_err(|_| overflow())?;
        if index >= self.size - 1 {
            return Ok((self.size - 2, Fixed::ONE));
        }
        let floor = Fixed::from_integer(whole)?;
        Ok((index, scaled.checked_sub(floor)?))
    }

    /// The four-vertex interpolation.
    ///
    /// The six branches are the six orderings of the three fractions, which is
    /// the same as saying which of the six tetrahedra of the cell the sample
    /// is in. Written out rather than computed, because the vertex sets are
    /// not a formula and a clever derivation of them is a place to be subtly
    /// wrong in one branch out of six — which shows as a fault in one eighth
    /// of one sixth of the colour space, and nowhere a still frame would
    /// reveal it.
    fn tetrahedral(&self, cell: [usize; 3], fraction: [Fixed; 3]) -> Result<Colour> {
        let [red, green, blue] = cell;
        let corner = |dr: usize, dg: usize, db: usize| self.sample(red + dr, green + dg, blue + db);
        let (fr, fg, fb) = (fraction[0], fraction[1], fraction[2]);
        let base = corner(0, 0, 0)?;

        // Each arm names three (weight, from, to) steps whose deltas telescope
        // from the near corner to the far one when the fractions are equal.
        let steps = if fr >= fg && fg >= fb {
            [
                (fr, corner(1, 0, 0)?, base),
                (fg, corner(1, 1, 0)?, corner(1, 0, 0)?),
                (fb, corner(1, 1, 1)?, corner(1, 1, 0)?),
            ]
        } else if fr >= fb && fb >= fg {
            [
                (fr, corner(1, 0, 0)?, base),
                (fg, corner(1, 1, 1)?, corner(1, 0, 1)?),
                (fb, corner(1, 0, 1)?, corner(1, 0, 0)?),
            ]
        } else if fb >= fr && fr >= fg {
            [
                (fr, corner(1, 0, 1)?, corner(0, 0, 1)?),
                (fg, corner(1, 1, 1)?, corner(1, 0, 1)?),
                (fb, corner(0, 0, 1)?, base),
            ]
        } else if fg >= fr && fr >= fb {
            [
                (fr, corner(1, 1, 0)?, corner(0, 1, 0)?),
                (fg, corner(0, 1, 0)?, base),
                (fb, corner(1, 1, 1)?, corner(1, 1, 0)?),
            ]
        } else if fg >= fb && fb >= fr {
            [
                (fr, corner(1, 1, 1)?, corner(0, 1, 1)?),
                (fg, corner(0, 1, 0)?, base),
                (fb, corner(0, 1, 1)?, corner(0, 1, 0)?),
            ]
        } else {
            [
                (fr, corner(1, 1, 1)?, corner(0, 1, 1)?),
                (fg, corner(0, 1, 1)?, corner(0, 0, 1)?),
                (fb, corner(0, 0, 1)?, base),
            ]
        };

        let mut out = base;
        for channel in 0..3 {
            for (weight, to, from) in &steps {
                let delta = to[channel].checked_sub(from[channel])?;
                out[channel] = out[channel].checked_add(delta.checked_mul(*weight)?)?;
            }
        }
        Ok(out)
    }

    /// The eight-corner interpolation.
    ///
    /// Present for comparison and for reproducing another system's output, not
    /// because it is the better answer. See this module's own documentation.
    fn trilinear(&self, cell: [usize; 3], fraction: [Fixed; 3]) -> Result<Colour> {
        let [red, green, blue] = cell;
        let mut out = [Fixed::ZERO; 3];
        for step in 0..8 {
            let dr = step & 1;
            let dg = (step >> 1) & 1;
            let db = (step >> 2) & 1;
            let mut weight = Fixed::ONE;
            for (offset, part) in [dr, dg, db].into_iter().zip(fraction) {
                let along = if offset == 1 {
                    part
                } else {
                    Fixed::ONE.checked_sub(part)?
                };
                weight = weight.checked_mul(along)?;
            }
            let corner = self.sample(red + dr, green + dg, blue + db)?;
            for channel in 0..3 {
                out[channel] = out[channel].checked_add(corner[channel].checked_mul(weight)?)?;
            }
        }
        Ok(out)
    }
}

/// Where a lattice index sits in nought to one.
fn lattice(index: usize, size: usize) -> Result<Fixed> {
    let numerator = i64::try_from(index).map_err(|_| overflow())?;
    let denominator = i64::try_from(size - 1).map_err(|_| overflow())?;
    Ok(Fixed::from_rational(media_editor_core::Rational::new(
        numerator,
        denominator,
    )?)?)
}

/// The refusal for arithmetic that will not fit.
fn overflow() -> RenderStatus {
    RenderStatus::Time(media_editor_core::CoreStatus::Overflow)
}
