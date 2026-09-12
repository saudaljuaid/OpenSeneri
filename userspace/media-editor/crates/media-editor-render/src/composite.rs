// SPDX-License-Identifier: GPL-3.0-only
//! Compositing: putting one picture over another.
//!
//! Two decisions have to be made before a single sample is touched, and
//! getting either wrong produces a picture that looks nearly right, which is
//! the worst kind of wrong.
//!
//! **In what space?** Alpha is coverage: the fraction of a pixel the top layer
//! actually occupies. What reaches the eye is that fraction of the top layer's
//! light plus the rest of the bottom layer's light, and that sentence is only
//! true of *light*. Adding gamma-encoded code values instead produces edges
//! that are too bright, which is why a white title over black looks like it
//! has a halo in one application and not another. So every sample here is
//! decoded to linear light, composited, and encoded back — regardless of what
//! the frames are encoded in. Nothing is refused for being sRGB; it is simply
//! not added in sRGB.
//!
//! **Straight or premultiplied?** `over` is only correct — and only
//! associative — on premultiplied values. Compositing straight samples as
//! though they were premultiplied is the dark fringe around every badly keyed
//! title: the edge pixels are half-covered, their colour is still full
//! strength, and adding the background underneath them makes them too dark.
//! A frame here says which kind it holds ([`AlphaState`]), and the two are not
//! interchangeable.
//!
//! Media Editor premultiplies **in linear light**. A premultiplied sample is the
//! encoding of `light × coverage`, not the encoded value scaled by coverage.
//! Both conventions exist in the wild and they disagree, so this one is
//! written down here and enforced by [`checked_premultiplied`] rather than
//! assumed.
//!
//! Only [`PixelFormat::Rgba8`] can be composited, because it is the only
//! format with an alpha channel. There is nothing to composite without one.

use alloc::vec::Vec;

use media_editor_core::Rational;
use media_editor_media::{AlphaState, Frame, FrameDescription, PixelFormat};

use crate::convert::{LEVELS, TransferTable};
use crate::status::{RenderStatus, Result};
use media_editor_core::{FRACTION_BITS, Fixed};

/// How many bytes one `Rgba8` pixel occupies.
const CHANNELS: usize = 4;

/// Which of those bytes is the alpha.
const ALPHA: usize = 3;

/// Coverage, as a fraction of one, for every byte value.
///
/// Alpha is not light and never passes through a transfer function: it is a
/// fraction of a pixel's area, so it is linear by definition. Nor does it obey
/// a limited range — a coverage of "sixteen" means nothing. Two hundred and
/// fifty-five is full coverage in every range this format admits.
fn coverage_table() -> Result<[Fixed; LEVELS]> {
    let mut table = [Fixed::ZERO; LEVELS];
    for (code, slot) in table.iter_mut().enumerate() {
        let code = i64::try_from(code).map_err(|_| RenderStatus::OutsideDomain)?;
        *slot = Fixed::from_rational(Rational::new(code, 255).map_err(RenderStatus::Time)?)?;
    }
    Ok(table)
}

/// The nearest byte to a coverage fraction.
fn quantise_coverage(coverage: Fixed) -> Result<u8> {
    let scaled = coverage.checked_mul(Fixed::from_integer(255)?)?;
    let half = 1_i64 << (FRACTION_BITS - 1);
    let rounded = scaled.raw().saturating_add(half) >> FRACTION_BITS;
    u8::try_from(rounded.clamp(0, 255)).map_err(|_| RenderStatus::OutsideDomain)
}

/// One, if the value is above it.
///
/// Compositing cannot produce more light than the two layers hold, but
/// quantisation can push the last code value a hair past full scale, and a
/// transfer function's domain stops there.
fn clamp_to_one(value: Fixed) -> Fixed {
    if value > Fixed::ONE {
        Fixed::ONE
    } else {
        value
    }
}

/// The description a frame must have to be composited, in the state given.
fn require(frame: &Frame, state: AlphaState) -> Result<FrameDescription> {
    let described = *frame.description();
    if described.format() != PixelFormat::Rgba8 {
        return Err(RenderStatus::AlphaRequired);
    }
    if described.alpha() != Some(state) {
        return Err(RenderStatus::WrongAlphaState);
    }
    Ok(described)
}

/// How far past the exact bound a re-encoded sample may sit.
///
/// One code value, and the reason is worth writing down because the exact
/// bound is so nearly right. A premultiplied sample is `light × coverage`, so
/// its light is at most the coverage, and quantisation is monotonic, so the
/// bound survives into the code domain untouched — for a frame that has only
/// ever been premultiplied or composited.
///
/// A frame that has been *re-encoded* is different. Decoding a code value
/// returns the light at the middle of its bucket, not the light that produced
/// it, so a sample sitting exactly on the bound can come back up to half a
/// step above it, and quantising that into a second table's different bucket
/// boundaries can land one code value past where it started. That is a
/// property of changing encodings, not a defect in the frame.
///
/// One code value is therefore allowed, and no more. It is measured rather
/// than assumed: [`crate::composite`]'s tests pin it from both sides, so a
/// sample two code values past the bound is still refused. The bug this check
/// exists to catch — straight samples relabelled as premultiplied — misses by
/// far more than one: full white at half coverage sits sixty-seven code values
/// above its ceiling.
const REQUANTISATION_SLACK: u8 = 1;

/// The highest code value a premultiplied sample may hold, per alpha byte.
fn ceiling_table(table: &TransferTable, coverage: &[Fixed; LEVELS]) -> [u8; LEVELS] {
    let mut ceiling = [0_u8; LEVELS];
    for (alpha, slot) in ceiling.iter_mut().enumerate() {
        *slot = table
            .encode(coverage[alpha])
            .saturating_add(REQUANTISATION_SLACK);
    }
    ceiling
}

/// Check that a frame calling itself premultiplied actually is.
///
/// A frame whose colour is brighter than its coverage has not been
/// premultiplied, whatever its description says — the straight samples were
/// simply relabelled. That is the dark-fringe bug arriving with a note saying
/// it is not one, so it is caught here rather than composited.
///
/// # Errors
///
/// [`RenderStatus::AlphaRequired`] for a format with no alpha channel,
/// [`RenderStatus::WrongAlphaState`] for a frame that does not claim to be
/// premultiplied, and [`RenderStatus::NotPremultiplied`] for one that claims
/// it and is not.
pub fn checked_premultiplied(frame: &Frame) -> Result<()> {
    let described = require(frame, AlphaState::Premultiplied)?;
    let table = TransferTable::build(described.colour())?;
    let coverage = coverage_table()?;
    let ceiling = ceiling_table(&table, &coverage);
    let samples = frame.packed()?;
    for pixel in samples.chunks_exact(CHANNELS) {
        let limit = ceiling[usize::from(pixel[ALPHA])];
        for &code in &pixel[..ALPHA] {
            if code > limit {
                return Err(RenderStatus::NotPremultiplied);
            }
        }
    }
    Ok(())
}

/// Multiply a straight frame's colour by its coverage, in linear light.
///
/// # Errors
///
/// [`RenderStatus::AlphaRequired`] for a format with no alpha channel,
/// [`RenderStatus::WrongAlphaState`] for a frame that is not straight, and
/// [`RenderStatus::OutOfMemory`] if the result cannot be held.
pub fn premultiply(frame: &Frame) -> Result<Frame> {
    let described = require(frame, AlphaState::Straight)?;
    let table = TransferTable::build(described.colour())?;
    let coverage = coverage_table()?;
    let samples = frame.packed()?;
    let mut out = reserved(samples.len())?;
    for pixel in samples.chunks_exact(CHANNELS) {
        let alpha = pixel[ALPHA];
        let fraction = coverage[usize::from(alpha)];
        for &code in &pixel[..ALPHA] {
            let light = table.decode(code).checked_mul(fraction)?;
            out.push(table.encode(light));
        }
        out.push(alpha);
    }
    Ok(Frame::from_owned(
        described.with_alpha(AlphaState::Premultiplied)?,
        out,
    )?)
}

/// Divide a premultiplied frame's colour back out of its coverage.
///
/// This is not the inverse of [`premultiply`] and cannot be. Multiplying by a
/// fraction throws away precision that dividing cannot bring back, so the
/// round trip is exact only where coverage is full, and at zero coverage the
/// colour is gone entirely — nothing was kept, so nothing is recovered, and
/// the result is black. That pixel is invisible either way.
///
/// # Errors
///
/// [`RenderStatus::AlphaRequired`] for a format with no alpha channel,
/// [`RenderStatus::WrongAlphaState`] for a frame that is not premultiplied,
/// and [`RenderStatus::OutOfMemory`] if the result cannot be held.
pub fn unpremultiply(frame: &Frame) -> Result<Frame> {
    let described = require(frame, AlphaState::Premultiplied)?;
    let table = TransferTable::build(described.colour())?;
    let coverage = coverage_table()?;
    let samples = frame.packed()?;
    let mut out = reserved(samples.len())?;
    for pixel in samples.chunks_exact(CHANNELS) {
        let alpha = pixel[ALPHA];
        let fraction = coverage[usize::from(alpha)];
        for &code in &pixel[..ALPHA] {
            let light = if alpha == 0 {
                Fixed::ZERO
            } else {
                clamp_to_one(table.decode(code).checked_div(fraction)?)
            };
            out.push(table.encode(light));
        }
        out.push(alpha);
    }
    Ok(Frame::from_owned(
        described.with_alpha(AlphaState::Straight)?,
        out,
    )?)
}

/// Put one frame over another.
///
/// Porter-Duff `over` on premultiplied values, in linear light:
///
/// ```text
/// colour = colour_top + colour_bottom × (1 − alpha_top)
/// alpha  = alpha_top  + alpha_bottom  × (1 − alpha_top)
/// ```
///
/// Both frames must be premultiplied, must actually be premultiplied
/// ([`checked_premultiplied`]), and must share a description exactly. Two
/// frames with different colour descriptions are not two layers of one picture
/// — they are two pictures, and adding them means deciding which one's
/// primaries the answer is in. That is [`crate::convert`]'s decision to make,
/// with a name on it, not a silent assumption here (R-8.3).
///
/// # Errors
///
/// [`RenderStatus::AlphaRequired`], [`RenderStatus::WrongAlphaState`] and
/// [`RenderStatus::NotPremultiplied`] as above,
/// [`RenderStatus::NotComposable`] if the descriptions differ, and
/// [`RenderStatus::OutOfMemory`] if the result cannot be held.
pub fn over(top: &Frame, bottom: &Frame) -> Result<Frame> {
    let described = require(top, AlphaState::Premultiplied)?;
    if *bottom.description() != described {
        require(bottom, AlphaState::Premultiplied)?;
        return Err(RenderStatus::NotComposable);
    }
    checked_premultiplied(top)?;
    checked_premultiplied(bottom)?;

    let table = TransferTable::build(described.colour())?;
    let coverage = coverage_table()?;
    let above = top.packed()?;
    let below = bottom.packed()?;
    let mut out = reserved(above.len())?;
    for (upper, lower) in above
        .chunks_exact(CHANNELS)
        .zip(below.chunks_exact(CHANNELS))
    {
        let alpha_top = coverage[usize::from(upper[ALPHA])];
        let remainder = Fixed::ONE.checked_sub(alpha_top)?;
        for channel in 0..ALPHA {
            let light = table
                .decode(lower[channel])
                .checked_mul(remainder)?
                .checked_add(table.decode(upper[channel]))?;
            out.push(table.encode(clamp_to_one(light)));
        }
        let alpha_bottom = coverage[usize::from(lower[ALPHA])];
        let alpha = alpha_bottom
            .checked_mul(remainder)?
            .checked_add(alpha_top)?;
        out.push(quantise_coverage(clamp_to_one(alpha))?);
    }
    Ok(Frame::from_owned(described, out)?)
}

/// A premultiplied frame behind a coverage plane.
///
/// This is [`faded`] with a different opacity at every pixel, and it is what a
/// **wipe** and a **mask** are both made of. A wipe therefore needs no
/// compositing operator of its own, for exactly the reason a dissolve does
/// not: mask the incoming layer and put it [`over`] the outgoing one, and the
/// result is `in x coverage + out x (1 - coverage)` — a cross-fade whose
/// fraction happens to vary across the picture instead of over time.
///
/// Every channel is scaled, coverage included, for the reason [`faded`] gives:
/// in premultiplied form the colour has already been multiplied by the
/// coverage, so a frame whose colour was scaled and whose alpha was not would
/// claim more colour than its coverage allows, and [`over`] would be right to
/// refuse it.
///
/// The plane is one byte per pixel in the frame's own order, which is what
/// [`crate::shape::plane`] produces.
///
/// # Errors
///
/// [`RenderStatus::AlphaRequired`] for a format with no alpha channel,
/// [`RenderStatus::WrongAlphaState`] for a frame that is not premultiplied,
/// [`RenderStatus::CoverageSizeMismatch`] for a plane that is not one byte per
/// pixel of this frame, and [`RenderStatus::OutOfMemory`].
pub fn masked(frame: &Frame, coverage: &[u8]) -> Result<Frame> {
    let described = require(frame, AlphaState::Premultiplied)?;
    let samples = frame.packed()?;
    if coverage.len().checked_mul(CHANNELS) != Some(samples.len()) {
        return Err(RenderStatus::CoverageSizeMismatch);
    }
    let table = TransferTable::build(described.colour())?;
    let fractions = coverage_table()?;
    let mut out = reserved(samples.len())?;
    for (pixel, ink) in samples.chunks_exact(CHANNELS).zip(coverage) {
        // Colour in light, coverage in its stored value -- the same division
        // [`faded`] makes and for the same reason. A mask that scaled its
        // colour in code values would darken every soft edge it drew by
        // exactly the amount the transfer curve bends, which reads as a fringe
        // rather than as a bug.
        let fraction = fractions[usize::from(*ink)];
        // Full coverage is a multiply by 255/255, which is exact, so an
        // unmasked pixel arrives unchanged rather than nearly unchanged.
        let scaled = i64::from(pixel[ALPHA]) * i64::from(*ink);
        let full = i64::from(u8::MAX);
        let masked_coverage = u8::try_from((scaled * 2 + full) / (full * 2)).unwrap_or(u8::MAX);
        let ceiling = fractions[usize::from(masked_coverage)];
        for &code in &pixel[..ALPHA] {
            let light = table.decode(code).checked_mul(fraction)?;
            out.push(table.encode(held_under(light, ceiling)));
        }
        out.push(masked_coverage);
    }
    Ok(Frame::from_owned(described, out)?)
}

/// A buffer of the right size, or a named refusal (R-5.3).
fn reserved(bytes: usize) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    out.try_reserve(bytes)
        .map_err(|_| RenderStatus::OutOfMemory)?;
    Ok(out)
}

/// A premultiplied frame at a fraction of its opacity.
///
/// Every channel, coverage included. In premultiplied form the colour has
/// already been multiplied by the coverage, so halving the coverage means
/// halving the colour too — anything else would leave a frame claiming more
/// colour than its coverage allows, which [`over`] refuses and is right to.
///
/// The **colour** is scaled in linear light and the **coverage** is scaled in
/// its stored value, and those are two different things because only one of
/// them is light. This is the correction of a real bug: the first version
/// scaled both in code values and defended it in a comment — "coverage is not
/// light, and a premultiplied colour sample is a coverage-weighted quantity"
/// — which is true of the coverage and false of the colour. A premultiplied
/// sample stores `encode(colour x coverage)`, and `encode(c) x t` is not
/// `encode(c x t)` for any transfer curve that bends.
///
/// What it cost: a dissolve between two *identical* pictures dipped by as much
/// as twenty-eight code values in the middle, which is a visible sag in the
/// middle of every dissolve between two shots of anything. Every test the
/// project had faded a layer that was **black**, where nought times anything
/// is nought and the two arithmetics agree — the third time that particular
/// blind spot has cost something here.
///
/// This is what makes a dissolve need no operator of its own: fade the
/// incoming layer and put it `over` the outgoing one, and the result is
/// `in x t + out x (1 - t)` — which is now true rather than nearly true.
///
/// # Errors
///
/// [`RenderStatus::AlphaRequired`] for a format with no alpha channel,
/// [`RenderStatus::WrongAlphaState`] for a frame that is not premultiplied,
/// [`RenderStatus::Time`] wrapping an overflow, and
/// [`RenderStatus::OutOfMemory`].
pub fn faded(frame: &Frame, opacity: Rational) -> Result<Frame> {
    let described = require(frame, AlphaState::Premultiplied)?;
    if opacity == Rational::ONE {
        // The overwhelmingly common case, and it must be a copy rather than a
        // multiply by one: a layer nobody is dissolving must arrive exactly.
        return Ok(frame.clone());
    }
    let table = TransferTable::build(described.colour())?;
    let fractions = coverage_table()?;
    let fraction = Fixed::from_rational(opacity).map_err(RenderStatus::Time)?;
    let samples = frame.packed()?;
    let mut out = reserved(samples.len())?;
    for pixel in samples.chunks_exact(CHANNELS) {
        let faded_coverage = scaled_coverage(pixel[ALPHA], opacity)?;
        let ceiling = fractions[usize::from(faded_coverage)];
        for &code in &pixel[..ALPHA] {
            let light = table.decode(code).checked_mul(fraction)?;
            out.push(table.encode(held_under(light, ceiling)));
        }
        out.push(faded_coverage);
    }
    Ok(Frame::from_owned(described, out)?)
}

/// A light value no brighter than the coverage carrying it.
///
/// Not a fudge and not a clamp for safety: this *is* what premultiplied means.
/// The colour is scaled through a curve and the coverage is scaled as an
/// integer, and the two roundings land a fraction apart — so a sample that was
/// exactly at its coverage can come out a code value above it, which
/// [`checked_premultiplied`] refuses and is right to. Enforcing the invariant
/// here is the whole of the fix, and it costs nothing when the sample is
/// anywhere below the limit, which is almost always.
fn held_under(light: Fixed, ceiling: Fixed) -> Fixed {
    let held = clamp_to_one(light);
    if held > ceiling { ceiling } else { held }
}

/// A coverage byte at a fraction of itself, exactly.
///
/// Integer arithmetic on the stored value, rounded half away from zero, so a
/// fade is the same fade on every machine (R-4.1). Coverage really *is* linear
/// in its code value — it is a fraction of a pixel, not an amount of light —
/// so this is the one channel that must not go through the transfer curve.
fn scaled_coverage(coverage: u8, fraction: Rational) -> Result<u8> {
    let numerator = fraction.numerator();
    let denominator = fraction.denominator();
    let scaled = i64::from(coverage)
        .checked_mul(numerator)
        .ok_or(RenderStatus::Time(media_editor_core::CoreStatus::Overflow))?;
    let rounded = (scaled * 2 + denominator) / (denominator * 2);
    Ok(u8::try_from(rounded.clamp(0, 255)).unwrap_or(255))
}
