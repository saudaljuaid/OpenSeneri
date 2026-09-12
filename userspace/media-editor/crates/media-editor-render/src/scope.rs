// SPDX-License-Identifier: GPL-3.0-only
//! Scopes: what a colourist actually looks at.
//!
//! A waveform and a histogram are measurements, not pictures. They are
//! computed from the same frames the viewer shows, by counting — which means
//! they are exact, they are integers, and two runs over one frame produce
//! identical scopes. There is nothing here to be approximately right about.
//!
//! The one judgement call is how to turn red, green and blue into luminance
//! for a waveform. For a luma-chroma frame there is nothing to decide: plane
//! zero *is* luma. For an RGB frame there are no matrix coefficients to use,
//! so the primaries decide, through the exact derivation in
//! [`crate::chromaticity`] — which is the right answer and not an
//! approximation of one.

use alloc::vec;
use alloc::vec::Vec;

use media_editor_core::Rational;
use media_editor_media::colour::MatrixCoefficients;
use media_editor_media::{ColourDescription, Frame, PixelFormat};

use crate::chromaticity::gamut_of;
use crate::status::{RenderStatus, Result};

/// How many code values an eight-bit scope counts over.
pub const LEVELS: usize = 256;

/// The scale the integer luma weights are expressed in.
///
/// Sixteen bits, so the weighted sum of three eight-bit samples stays inside a
/// `u32` with room to spare, and the rounding is one shift.
pub const WEIGHT_SCALE: u32 = 1 << 16;

/// How much each of red, green and blue contributes to luminance, as integers
/// that sum to exactly [`WEIGHT_SCALE`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LumaWeights {
    red: u32,
    green: u32,
    blue: u32,
}

impl LumaWeights {
    /// The weights for a colour description.
    ///
    /// For a luma-chroma matrix these are the coefficients the standard
    /// specifies. For an identity matrix — an RGB frame, which has no matrix
    /// coefficients — they are derived from the primaries exactly.
    ///
    /// BT.601 is why the two are kept apart: its matrix uses coefficients that
    /// came from the 1953 primaries and were kept when the primaries changed,
    /// so deriving them from its modern primaries would give a different
    /// measurement from the one a broadcast waveform monitor shows.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Time`] wrapping an arithmetic refusal, or
    /// [`RenderStatus::DegenerateGamut`].
    pub fn of(colour: ColourDescription) -> Result<Self> {
        Self::from_exact(luma_coefficients(colour)?)
    }

    /// Round three exact weights into integers that sum to exactly the scale.
    ///
    /// Red and blue are rounded to nearest; green takes whatever is left. That
    /// is not laziness — the three must sum to the scale or a white field
    /// would not measure as white, and green is the largest of the three, so
    /// giving it the remainder is the smallest relative disturbance.
    fn from_exact(exact: [Rational; 3]) -> Result<Self> {
        let red = round_to_scale(exact[0])?;
        let blue = round_to_scale(exact[2])?;
        let green = WEIGHT_SCALE
            .checked_sub(red)
            .and_then(|left| left.checked_sub(blue))
            .ok_or(RenderStatus::DegenerateGamut)?;
        Ok(Self { red, green, blue })
    }

    /// The red weight.
    #[must_use]
    pub const fn red(self) -> u32 {
        self.red
    }

    /// The green weight.
    #[must_use]
    pub const fn green(self) -> u32 {
        self.green
    }

    /// The blue weight.
    #[must_use]
    pub const fn blue(self) -> u32 {
        self.blue
    }

    /// The luminance of one colour, rounded to nearest.
    #[must_use]
    pub fn luma_of(self, red: u8, green: u8, blue: u8) -> u8 {
        let sum =
            u32::from(red) * self.red + u32::from(green) * self.green + u32::from(blue) * self.blue;
        // Round to nearest so that white measures 255 rather than 254.
        let scaled = (sum + WEIGHT_SCALE / 2) / WEIGHT_SCALE;
        u8::try_from(scaled.min(255)).unwrap_or(255)
    }
}

/// How much each primary contributes to luminance, exactly.
///
/// For a luma-chroma matrix these are the coefficients the standard specifies.
/// For an identity matrix — an RGB frame, which has no matrix coefficients —
/// they are derived from the primaries.
///
/// # Errors
///
/// [`RenderStatus::Time`] wrapping an arithmetic refusal, or
/// [`RenderStatus::DegenerateGamut`].
pub fn luma_coefficients(colour: ColourDescription) -> Result<[Rational; 3]> {
    let exact = match colour.matrix {
        MatrixCoefficients::Bt709 => [
            Rational::new(2126, 10_000).map_err(RenderStatus::Time)?,
            Rational::new(7152, 10_000).map_err(RenderStatus::Time)?,
            Rational::new(722, 10_000).map_err(RenderStatus::Time)?,
        ],
        MatrixCoefficients::Bt601 => [
            Rational::new(299, 1000).map_err(RenderStatus::Time)?,
            Rational::new(587, 1000).map_err(RenderStatus::Time)?,
            Rational::new(114, 1000).map_err(RenderStatus::Time)?,
        ],
        MatrixCoefficients::Bt2020NonConstant => [
            Rational::new(2627, 10_000).map_err(RenderStatus::Time)?,
            Rational::new(6780, 10_000).map_err(RenderStatus::Time)?,
            Rational::new(593, 10_000).map_err(RenderStatus::Time)?,
        ],
        MatrixCoefficients::Identity => {
            let derived = gamut_of(colour.primaries)?.derived_luminance()?;
            *derived.elements()
        }
    };
    Ok(exact)
}

/// Convert an exact weight into the integer scale, rounded to nearest.
fn round_to_scale(value: Rational) -> Result<u32> {
    let scale = i64::from(WEIGHT_SCALE);
    let half = Rational::new(1, 2).map_err(RenderStatus::Time)?;
    let scaled = value
        .scale(scale)
        .and_then(|value| value.checked_add(half))
        .and_then(Rational::floor)
        .map_err(RenderStatus::Time)?;
    u32::try_from(scaled).map_err(|_| RenderStatus::DegenerateGamut)
}

/// How many samples a frame holds at each code value, per channel.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Histogram {
    channels: usize,
    counts: Vec<u64>,
}

impl Histogram {
    /// How many channels were counted.
    #[must_use]
    pub const fn channels(&self) -> usize {
        self.channels
    }

    /// How many samples of one channel had one code value.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::OutsideDomain`] if the channel or level names nothing.
    pub fn count(&self, channel: usize, level: usize) -> Result<u64> {
        if channel >= self.channels || level >= LEVELS {
            return Err(RenderStatus::OutsideDomain);
        }
        self.counts
            .get(channel * LEVELS + level)
            .copied()
            .ok_or(RenderStatus::OutsideDomain)
    }

    /// The total number of samples counted in one channel.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::OutsideDomain`] if the channel names nothing.
    pub fn total(&self, channel: usize) -> Result<u64> {
        if channel >= self.channels {
            return Err(RenderStatus::OutsideDomain);
        }
        let start = channel * LEVELS;
        Ok(self
            .counts
            .get(start..start + LEVELS)
            .ok_or(RenderStatus::OutsideDomain)?
            .iter()
            .sum())
    }
}

/// Count a frame's samples, channel by channel.
///
/// A packed format is counted per component in storage order, so an RGBA frame
/// yields four channels; a planar format yields one per plane. Alpha is
/// counted because a colourist who cannot see the alpha cannot see why the
/// picture is wrong.
///
/// # Errors
///
/// [`RenderStatus::Media`] if the frame's planes disagree with its
/// description, or [`RenderStatus::OutOfMemory`].
pub fn histogram(frame: &Frame) -> Result<Histogram> {
    let description = frame.description();
    let format = description.format();
    let channels = match format {
        PixelFormat::Rgba8 => 4,
        PixelFormat::Gray8 => 1,
        // Three either way, but for different reasons: red, green and blue for
        // one, and luma with its two chroma planes for the others.
        PixelFormat::Rgb8
        | PixelFormat::Yuv420p8
        | PixelFormat::Yuv422p8
        | PixelFormat::Yuv444p8 => 3,
    };

    let mut counts = Vec::new();
    counts
        .try_reserve(channels * LEVELS)
        .map_err(|_| RenderStatus::OutOfMemory)?;
    counts.resize(channels * LEVELS, 0_u64);

    if format.is_rgb() {
        let plane = frame.plane(0).map_err(RenderStatus::Media)?;
        let row_bytes = format
            .plane_row_bytes(description.geometry(), 0)
            .map_err(RenderStatus::Media)?;
        for line in 0..description.geometry().height() {
            let line = usize::try_from(line).map_err(|_| RenderStatus::OutsideDomain)?;
            let row = plane.row(line).map_err(RenderStatus::Media)?;
            let row = row.get(..row_bytes).ok_or(RenderStatus::OutsideDomain)?;
            for pixel in row.chunks_exact(channels) {
                for (channel, sample) in pixel.iter().enumerate() {
                    counts[channel * LEVELS + usize::from(*sample)] += 1;
                }
            }
        }
    } else {
        for channel in 0..channels {
            let plane = frame.plane(channel).map_err(RenderStatus::Media)?;
            let plane_geometry = format
                .plane_geometry(description.geometry(), channel)
                .map_err(RenderStatus::Media)?;
            let row_bytes = format
                .plane_row_bytes(description.geometry(), channel)
                .map_err(RenderStatus::Media)?;
            for line in 0..plane_geometry.height() {
                let line = usize::try_from(line).map_err(|_| RenderStatus::OutsideDomain)?;
                let row = plane.row(line).map_err(RenderStatus::Media)?;
                let row = row.get(..row_bytes).ok_or(RenderStatus::OutsideDomain)?;
                for sample in row {
                    counts[channel * LEVELS + usize::from(*sample)] += 1;
                }
            }
        }
    }

    Ok(Histogram { channels, counts })
}

/// A waveform: for each column of the picture, how many of its samples sat at
/// each level.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Waveform {
    columns: usize,
    cells: Vec<u32>,
}

impl Waveform {
    /// How many columns the waveform has, which is the picture's width.
    #[must_use]
    pub const fn columns(&self) -> usize {
        self.columns
    }

    /// How many samples in one column sat at one level.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::OutsideDomain`] if the column or level names nothing.
    pub fn count(&self, column: usize, level: usize) -> Result<u32> {
        if column >= self.columns || level >= LEVELS {
            return Err(RenderStatus::OutsideDomain);
        }
        self.cells
            .get(column * LEVELS + level)
            .copied()
            .ok_or(RenderStatus::OutsideDomain)
    }

    /// The highest level any sample in a column reached.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::OutsideDomain`] if the column names nothing.
    pub fn peak(&self, column: usize) -> Result<Option<usize>> {
        if column >= self.columns {
            return Err(RenderStatus::OutsideDomain);
        }
        let start = column * LEVELS;
        let cells = self
            .cells
            .get(start..start + LEVELS)
            .ok_or(RenderStatus::OutsideDomain)?;
        Ok(cells
            .iter()
            .enumerate()
            .rfind(|(_, count)| **count > 0)
            .map(|(level, _)| level))
    }
}

/// Build a luminance waveform from a frame.
///
/// For a luma-chroma frame this reads plane zero, because that plane already
/// is luma. For an RGB frame it computes luminance with the weights
/// [`LumaWeights::of`] derives.
///
/// # Errors
///
/// [`RenderStatus::Media`], [`RenderStatus::OutOfMemory`], or a refusal from
/// the weight derivation.
pub fn waveform(frame: &Frame) -> Result<Waveform> {
    let description = frame.description();
    let format = description.format();
    let width =
        usize::try_from(description.geometry().width()).map_err(|_| RenderStatus::OutsideDomain)?;

    let mut cells = vec![];
    cells
        .try_reserve(width * LEVELS)
        .map_err(|_| RenderStatus::OutOfMemory)?;
    cells.resize(width * LEVELS, 0_u32);

    let plane = frame.plane(0).map_err(RenderStatus::Media)?;
    let row_bytes = format
        .plane_row_bytes(description.geometry(), 0)
        .map_err(RenderStatus::Media)?;
    let components = match format {
        PixelFormat::Rgba8 => 4,
        PixelFormat::Rgb8 => 3,
        _ => 1,
    };
    let weights = if format.is_rgb() && components > 1 {
        Some(LumaWeights::of(description.colour())?)
    } else {
        None
    };

    for line in 0..description.geometry().height() {
        let line = usize::try_from(line).map_err(|_| RenderStatus::OutsideDomain)?;
        let row = plane.row(line).map_err(RenderStatus::Media)?;
        let row = row.get(..row_bytes).ok_or(RenderStatus::OutsideDomain)?;
        for (column, pixel) in row.chunks_exact(components).enumerate() {
            let level = match weights {
                Some(weights) => usize::from(weights.luma_of(pixel[0], pixel[1], pixel[2])),
                None => usize::from(pixel[0]),
            };
            cells[column * LEVELS + level] += 1;
        }
    }

    Ok(Waveform {
        columns: width,
        cells,
    })
}
