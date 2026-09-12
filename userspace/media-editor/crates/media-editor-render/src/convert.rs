// SPDX-License-Identifier: GPL-3.0-only
//! Converting a frame from one description to another.
//!
//! This is where the exact matrices and the integer transfer functions stop
//! being arithmetic and start being pixels. The order is the one colour
//! science requires and the one most pipelines get wrong:
//!
//! ```text
//!   code values  →  normalise by range
//!                →  luma-chroma to red-green-blue, if the source has one
//!                →  decode the transfer, to linear light
//!                →  change gamut, in linear light and nowhere else
//!                →  encode the target's transfer
//!                →  red-green-blue to luma-chroma, if the target wants one
//!                →  quantise by range
//! ```
//!
//! Changing gamut anywhere but in linear light is the classic mistake: a
//! matrix applied to gamma-encoded values is not a gamut conversion, it is a
//! different picture that happens to look nearly right on a monitor.
//!
//! Two bounded contracts, stated rather than worked around (R-1.1, R-1.2).
//! The conversion refuses to change chroma subsampling, because that is a
//! resampling filter and a filter is a decision with a name; and it refuses a
//! geometry change, because that is a scaler. Both are separate operations
//! that will arrive with their own contracts.

use alloc::vec::Vec;

use media_editor_core::Rational;
use media_editor_media::{ColourDescription, Frame, FrameDescription, PixelFormat, Range};

use crate::chromaticity::gamut_of;
use crate::matrix::Matrix3;
use crate::scope::luma_coefficients;
use crate::status::{RenderStatus, Result};
use crate::transfer;
use media_editor_core::Fixed;

/// How many code values an eight-bit sample takes.
pub(crate) const LEVELS: usize = 256;

/// How far apart two pixels' alpha bytes are in the only format that has one.
const ALPHA_STRIDE: usize = 4;

/// Where the alpha byte sits within such a pixel.
const ALPHA_OFFSET: usize = 3;

/// A table from code value to linear light, built once per conversion.
///
/// A transfer function costs a logarithm and an exponential, which is far too
/// much to pay per sample. It is paid two hundred and fifty-six times instead,
/// and every sample after that is an index.
pub(crate) struct TransferTable {
    linear: [Fixed; LEVELS],
    low: usize,
    high: usize,
}

impl TransferTable {
    /// Build the table for a description's transfer function and range.
    ///
    /// The legal code values are part of the table, not an afterthought.
    /// Limited range does not merely scale differently — it *forbids* the
    /// values outside sixteen to two hundred and thirty-five, and a table that
    /// searched the whole byte would happily answer black with zero, which is
    /// an illegal sample that some equipment reads as a sync pattern.
    pub(crate) fn build(colour: ColourDescription) -> Result<Self> {
        let mut linear = [Fixed::ZERO; LEVELS];
        for (code, slot) in linear.iter_mut().enumerate() {
            let code = u8::try_from(code).map_err(|_| RenderStatus::OutsideDomain)?;
            let normalised = normalise(code, colour.range, false)?;
            *slot = transfer::decode(colour.transfer, normalised.max(Fixed::ZERO))?;
        }
        let (low, high) = legal_codes(colour.range, false);
        Ok(Self {
            linear,
            low: usize::try_from(low).unwrap_or(0),
            high: usize::try_from(high).unwrap_or(LEVELS - 1),
        })
    }

    /// The light a code value stands for.
    pub(crate) fn decode(&self, code: u8) -> Fixed {
        self.linear[usize::from(code)]
    }

    /// The code value whose light is nearest to this one.
    ///
    /// Quantisation is exactly this question, so it is answered exactly rather
    /// than by encoding and rounding: a binary search over the table, then a
    /// comparison of the two neighbours. The table rises, so the search is
    /// sound, and the answer is the same on every machine.
    pub(crate) fn encode(&self, light: Fixed) -> u8 {
        let mut low = self.low;
        let mut high = self.high;
        while low < high {
            let middle = low + (high - low) / 2;
            if self.linear[middle] < light {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        // `low` is the first legal entry at or above the value. Its neighbour
        // below may be nearer, if there is one inside the legal range.
        if low > self.low {
            let above = distance(self.linear[low], light);
            let below = distance(self.linear[low - 1], light);
            if below <= above {
                low -= 1;
            }
        }
        u8::try_from(low).unwrap_or(255)
    }
}

/// The absolute difference between two values, saturating rather than
/// overflowing on the extremes.
fn distance(left: Fixed, right: Fixed) -> i64 {
    left.raw().saturating_sub(right.raw()).saturating_abs()
}

/// Turn a code value into a normalised sample.
///
/// Luma and red-green-blue run from zero to one; chroma runs from minus a half
/// to a half, because that is what the matrices below expect.
pub(crate) fn normalise(code: u8, range: Range, chroma: bool) -> Result<Fixed> {
    let (offset, span) = match (range, chroma) {
        (Range::Full, false) => (0_i64, 255_i64),
        (Range::Full, true) => (128, 255),
        (Range::Limited, false) => (16, 219),
        (Range::Limited, true) => (128, 224),
    };
    let value = Rational::new(i64::from(code) - offset, span).map_err(RenderStatus::Time)?;
    Ok(Fixed::from_rational(value)?)
}

/// The lowest and highest code values a range permits.
const fn legal_codes(range: Range, chroma: bool) -> (i64, i64) {
    match (range, chroma) {
        (Range::Full, _) => (0, 255),
        (Range::Limited, false) => (16, 235),
        (Range::Limited, true) => (16, 240),
    }
}

/// Turn a normalised sample back into a code value, rounding to nearest and
/// clamping to what the range allows.
pub(crate) fn quantise(value: Fixed, range: Range, chroma: bool) -> Result<u8> {
    let (offset, span) = match (range, chroma) {
        (Range::Full, false) => (0_i64, 255_i64),
        (Range::Full, true) => (128, 255),
        (Range::Limited, false) => (16, 219),
        (Range::Limited, true) => (128, 224),
    };
    let (low, high) = legal_codes(range, chroma);
    let scaled = value.checked_mul(Fixed::from_integer(span)?)?;
    let half = Fixed::from_rational(Rational::new(1, 2).map_err(RenderStatus::Time)?)?;
    let rounded = scaled.checked_add(half)?.raw() >> media_editor_core::FRACTION_BITS;
    let code = (rounded + offset).clamp(low, high);
    u8::try_from(code).map_err(|_| RenderStatus::OutsideDomain)
}

/// The matrix that takes luma and chroma to red, green and blue.
fn ycbcr_to_rgb(colour: ColourDescription) -> Result<Matrix3> {
    let [red, green, blue] = luma_coefficients(colour)?;
    let two = Rational::from_integer(2);
    let red_span = two
        .checked_mul(Rational::ONE.checked_sub(red).map_err(RenderStatus::Time)?)
        .map_err(RenderStatus::Time)?;
    let blue_span = two
        .checked_mul(
            Rational::ONE
                .checked_sub(blue)
                .map_err(RenderStatus::Time)?,
        )
        .map_err(RenderStatus::Time)?;
    if green.is_zero() {
        return Err(RenderStatus::DegenerateGamut);
    }
    let green_per_red_difference = red_span
        .checked_mul(red)
        .and_then(|value| value.checked_div(green))
        .map_err(RenderStatus::Time)?
        .checked_neg()
        .map_err(RenderStatus::Time)?;
    let green_per_blue_difference = blue_span
        .checked_mul(blue)
        .and_then(|value| value.checked_div(green))
        .map_err(RenderStatus::Time)?
        .checked_neg()
        .map_err(RenderStatus::Time)?;
    Ok(Matrix3::from_rows([
        [Rational::ONE, Rational::ZERO, red_span],
        [
            Rational::ONE,
            green_per_blue_difference,
            green_per_red_difference,
        ],
        [Rational::ONE, blue_span, Rational::ZERO],
    ]))
}

/// The matrix that takes red, green and blue to luma and chroma.
pub(crate) fn rgb_to_ycbcr(colour: ColourDescription) -> Result<Matrix3> {
    ycbcr_to_rgb(colour)?.inverse()
}

/// A matrix of exact rationals, ready for per-sample arithmetic.
fn to_fixed(matrix: &Matrix3) -> Result<[[Fixed; 3]; 3]> {
    let mut out = [[Fixed::ZERO; 3]; 3];
    for (row, source) in matrix.rows().iter().enumerate() {
        for (column, value) in source.iter().enumerate() {
            out[row][column] = Fixed::from_rational(*value)?;
        }
    }
    Ok(out)
}

/// Apply a fixed-point matrix to three samples.
fn apply(matrix: &[[Fixed; 3]; 3], input: [Fixed; 3]) -> Result<[Fixed; 3]> {
    let mut out = [Fixed::ZERO; 3];
    for (row, coefficients) in matrix.iter().enumerate() {
        let mut sum = Fixed::ZERO;
        for (coefficient, sample) in coefficients.iter().zip(input.iter()) {
            sum = sum.checked_add(coefficient.checked_mul(*sample)?)?;
        }
        out[row] = sum;
    }
    Ok(out)
}

/// How many components a packed format interleaves, or one for a planar one.
const fn components(format: PixelFormat) -> usize {
    match format {
        PixelFormat::Rgba8 => 4,
        PixelFormat::Rgb8 => 3,
        _ => 1,
    }
}

/// Read one source pixel as linear light, whatever shape the source is in.
fn source_pixel(
    packed: &[u8],
    table: &TransferTable,
    plane_stride: usize,
    source: FrameDescription,
    from_ycbcr: Option<&[[Fixed; 3]; 3]>,
    index: usize,
) -> Result<[Fixed; 3]> {
    let planar = !source.format().is_rgb();
    let count = components(source.format());
    let sample = |offset: usize| -> Result<u8> {
        let position = if planar {
            offset * plane_stride + index
        } else {
            index * count + offset
        };
        packed
            .get(position)
            .copied()
            .ok_or(RenderStatus::OutsideDomain)
    };

    let Some(matrix) = from_ycbcr else {
        return Ok(if count == 1 {
            let value = table.decode(sample(0)?);
            [value, value, value]
        } else {
            [
                table.decode(sample(0)?),
                table.decode(sample(1)?),
                table.decode(sample(2)?),
            ]
        });
    };

    // Luma and chroma become red, green and blue *before* the transfer is
    // decoded: the matrix is defined on the encoded signal, not on light.
    let luma = normalise(sample(0)?, source.colour().range, false)?;
    let blue = normalise(sample(1)?, source.colour().range, true)?;
    let red = normalise(sample(2)?, source.colour().range, true)?;
    let rgb = apply(matrix, [luma, blue, red])?;
    Ok([
        transfer::decode(source.colour().transfer, rgb[0].max(Fixed::ZERO))?,
        transfer::decode(source.colour().transfer, rgb[1].max(Fixed::ZERO))?,
        transfer::decode(source.colour().transfer, rgb[2].max(Fixed::ZERO))?,
    ])
}

/// Convert a frame to another description.
///
/// The order is the one colour science requires: normalise, matrix out of
/// luma-chroma if there is one, decode to linear light, change gamut *in
/// linear light*, encode, matrix back into luma-chroma if the target wants
/// one, quantise.
///
/// # Errors
///
/// [`RenderStatus::ConversionUnavailable`] if the geometry or the chroma
/// subsampling would change — a scaler and a chroma filter are separate
/// operations with their own contracts, and guessing at either would be
/// repair rather than conversion (R-1.3). Otherwise any refusal from the
/// colour arithmetic.
pub fn convert(frame: &Frame, target: FrameDescription) -> Result<Frame> {
    let source = *frame.description();
    if source.geometry() != target.geometry() {
        return Err(RenderStatus::ConversionUnavailable);
    }
    if source.format().is_subsampled() || target.format().is_subsampled() {
        // A subsampled frame converts only to itself, and that is not a
        // conversion. Everything else needs a chroma filter.
        return Err(RenderStatus::ConversionUnavailable);
    }
    if source.alpha() != target.alpha() {
        // Gaining, losing, or re-associating alpha are three separate
        // operations with three separate names — `composite::premultiply`,
        // `composite::unpremultiply`, and `composite::over` against a
        // background. A colour conversion that quietly did any of them would
        // be the reason a keyed title arrives opaque (R-1.3).
        return Err(RenderStatus::ConversionUnavailable);
    }

    let source_table = TransferTable::build(source.colour())?;
    let target_table = TransferTable::build(target.colour())?;

    let from_ycbcr = if source.format().is_rgb() {
        None
    } else {
        Some(to_fixed(&ycbcr_to_rgb(source.colour())?)?)
    };
    let to_ycbcr = if target.format().is_rgb() {
        None
    } else {
        Some(to_fixed(&rgb_to_ycbcr(target.colour())?)?)
    };
    let gamut = if source.colour().primaries == target.colour().primaries {
        None
    } else {
        let matrix =
            gamut_of(source.colour().primaries)?.to_gamut(&gamut_of(target.colour().primaries)?)?;
        Some(to_fixed(&matrix)?)
    };

    let width =
        usize::try_from(source.geometry().width()).map_err(|_| RenderStatus::OutsideDomain)?;
    let height =
        usize::try_from(source.geometry().height()).map_err(|_| RenderStatus::OutsideDomain)?;
    let pixels = width
        .checked_mul(height)
        .ok_or(RenderStatus::OutsideDomain)?;

    let packed = frame.packed().map_err(RenderStatus::Media)?;

    // One pass over the source produces the whole frame in linear light, and
    // one pass over that writes the target's layout. Holding a frame of light
    // costs twenty-four bytes a pixel; a streaming version becomes worth
    // writing when frames are large enough for that to matter, and that is
    // `PHIP-03`'s problem before it is this function's.
    let mut light = Vec::new();
    light
        .try_reserve(pixels)
        .map_err(|_| RenderStatus::OutOfMemory)?;
    for index in 0..pixels {
        let linear = source_pixel(
            &packed,
            &source_table,
            pixels,
            source,
            from_ycbcr.as_ref(),
            index,
        )?;
        light.push(match gamut {
            Some(matrix) => apply(&matrix, linear)?,
            None => linear,
        });
    }

    let out = write_target(&light, &packed, target, &target_table, to_ycbcr.as_ref())?;

    Frame::from_owned(target, out).map_err(RenderStatus::Media)
}

/// Write a frame of linear light into the target's layout.
///
/// The second of `convert`'s two passes, and a separate job: everything above
/// decides what the light *is*, and everything here decides how it is written
/// down — encoded, matrixed back into luma-chroma if the target wants one,
/// quantised into the target's legal range, and interleaved or planar as the
/// format says.
fn write_target(
    light: &[[Fixed; 3]],
    packed: &[u8],
    target: FrameDescription,
    target_table: &TransferTable,
    to_ycbcr: Option<&[[Fixed; 3]; 3]>,
) -> Result<Vec<u8>> {
    let pixels = light.len();
    let mut out = Vec::new();
    out.try_reserve(target.packed_bytes().map_err(RenderStatus::Media)?)
        .map_err(|_| RenderStatus::OutOfMemory)?;

    if let Some(matrix) = to_ycbcr {
        // Planar output: luma first, then each chroma plane, so the result is
        // plane-ordered the way the format says.
        let mut encoded = Vec::new();
        encoded
            .try_reserve(pixels)
            .map_err(|_| RenderStatus::OutOfMemory)?;
        for linear in light {
            let signal = [
                transfer::encode(target.colour().transfer, linear[0].max(Fixed::ZERO))?,
                transfer::encode(target.colour().transfer, linear[1].max(Fixed::ZERO))?,
                transfer::encode(target.colour().transfer, linear[2].max(Fixed::ZERO))?,
            ];
            encoded.push(apply(matrix, signal)?);
        }
        for plane in 0..3_usize {
            for value in &encoded {
                out.push(quantise(value[plane], target.colour().range, plane != 0)?);
            }
        }
    } else {
        let count = components(target.format());
        for (index, linear) in light.iter().enumerate() {
            if count == 1 {
                // A single-channel target is luminance, not the red channel.
                let mut sum = Fixed::ZERO;
                for (weight, value) in luma_coefficients(target.colour())?
                    .iter()
                    .zip(linear.iter())
                {
                    sum = sum.checked_add(Fixed::from_rational(*weight)?.checked_mul(*value)?)?;
                }
                out.push(target_table.encode(sum));
            } else {
                for value in linear.iter().take(3) {
                    out.push(target_table.encode(*value));
                }
                if count == 4 {
                    // Coverage is not light. It passes through untouched: no
                    // transfer function, no gamut matrix, no quantisation to a
                    // limited range. Writing a constant here instead is how
                    // every pixel of a converted frame becomes opaque.
                    out.push(
                        *packed
                            .get(index * ALPHA_STRIDE + ALPHA_OFFSET)
                            .ok_or(RenderStatus::OutsideDomain)?,
                    );
                }
            }
        }
    }
    Ok(out)
}

/// The gamut matrix between two descriptions, for callers that want to inspect
/// it rather than apply it.
///
/// # Errors
///
/// As [`crate::Gamut::to_gamut`].
pub fn gamut_matrix(source: ColourDescription, target: ColourDescription) -> Result<Matrix3> {
    gamut_of(source.primaries)?.to_gamut(&gamut_of(target.primaries)?)
}
