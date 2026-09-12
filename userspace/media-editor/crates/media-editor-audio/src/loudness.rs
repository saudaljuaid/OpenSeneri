// SPDX-License-Identifier: GPL-3.0-only
//! ITU-R BS.1770 and EBU R 128 loudness measurement.
//!
//! Audio passes through the standard K-weighting filters, then weighted channel
//! mean squares are converted with `-0.691 + 10 log10(sum)`. Integrated
//! loudness uses 400 ms blocks with 75% overlap, a -70 LUFS absolute gate, and
//! a relative gate 10 LU below the ungated mean. Filter arithmetic uses
//! [`media_editor_core::WIDE_BITS`] fractional bits.

use alloc::vec::Vec;

use media_editor_core::{Fixed, Rational, WIDE_BITS};

use crate::buffer::{AudioBuffer, SampleRate};
use crate::status::{AudioStatus, Result};

/// How many samples a gating block holds, at the one rate this measures.
///
/// Four hundred milliseconds at 48 kHz, which is 19,200 samples exactly.
pub const BLOCK_SAMPLES: usize = 19_200;

/// How far apart two blocks begin.
///
/// A quarter of a block, which is the three-quarters overlap BS.1770 asks for.
pub const BLOCK_STEP: usize = BLOCK_SAMPLES / 4;

/// How many steps make a block.
///
/// Four, because the overlap is three quarters. It is written as the ratio of
/// the two rather than as `4` so that the two constants cannot drift apart:
/// if the overlap ever changes, this follows it.
const STEPS_PER_BLOCK: usize = BLOCK_SAMPLES / BLOCK_STEP;

/// The most channels this measures.
///
/// Two. BS.1770 weights the surround channels louder than the front ones,
/// which needs to know *which* channel is which — and an [`AudioBuffer`]
/// carries a channel count, not a layout. Inventing an order here would be
/// guessing at what the material is (R-1.3), so more than two is refused until
/// there is a layout to read.
pub const MAX_WEIGHTED_CHANNELS: usize = 2;

/// The absolute gate, in LUFS.
///
/// Below this a block is silence for the purposes of the measurement, whatever
/// it holds.
pub const ABSOLUTE_GATE: i64 = -70;

/// How far below the ungated mean the relative gate sits, in loudness units.
pub const RELATIVE_GATE: i64 = -10;

/// One biquad, as a difference equation with its coefficients exact.
struct Biquad {
    b: [i128; 3],
    a: [i128; 2],
    x: [i128; 2],
    y: [i128; 2],
}

impl Biquad {
    /// A section from the decimals a standard prints.
    fn new(b: [(i64, i64); 3], a: [(i64, i64); 2]) -> Result<Self> {
        let mut held_b = [0_i128; 3];
        let mut held_a = [0_i128; 2];
        for (slot, (numerator, denominator)) in held_b.iter_mut().zip(b) {
            *slot = wide_of(numerator, denominator)?;
        }
        for (slot, (numerator, denominator)) in held_a.iter_mut().zip(a) {
            *slot = wide_of(numerator, denominator)?;
        }
        Ok(Self {
            b: held_b,
            a: held_a,
            x: [0; 2],
            y: [0; 2],
        })
    }

    /// One sample through the section.
    fn step(&mut self, input: i128) -> Result<i128> {
        // Written as a sequence rather than a chain, because every term needs
        // its own multiply and `?` cannot cross a closure that returns an
        // option.
        let overflow = AudioStatus::Time(media_editor_core::CoreStatus::Overflow);
        let mut total = wide_mul(self.b[0], input)?;
        for (coefficient, sample) in [(self.b[1], self.x[0]), (self.b[2], self.x[1])] {
            total = total
                .checked_add(wide_mul(coefficient, sample)?)
                .ok_or(overflow)?;
        }
        for (coefficient, sample) in [(self.a[0], self.y[0]), (self.a[1], self.y[1])] {
            total = total
                .checked_sub(wide_mul(coefficient, sample)?)
                .ok_or(overflow)?;
        }
        self.x[1] = self.x[0];
        self.x[0] = input;
        self.y[1] = self.y[0];
        self.y[0] = total;
        Ok(total)
    }
}

/// The K-weighting filter: the two sections BS.1770-4 specifies, in order.
struct Weighting {
    shelf: Biquad,
    highpass: Biquad,
}

impl Weighting {
    /// The filter at 48 kHz, with the coefficients the standard prints.
    ///
    /// These are the *published* values, not values derived here. BS.1770-4
    /// gives them to fourteen decimal places for 48 kHz and says to re-derive
    /// the filter at other rates; this build measures at 48 kHz and refuses
    /// the rest rather than reusing coefficients that would be wrong.
    fn new() -> Result<Self> {
        Ok(Self {
            // Stage 1: the head shelf.
            shelf: Biquad::new(
                [
                    (153_512_485_958_697, 100_000_000_000_000),
                    (-269_169_618_940_638, 100_000_000_000_000),
                    (119_839_281_085_285, 100_000_000_000_000),
                ],
                [
                    (-169_065_929_318_241, 100_000_000_000_000),
                    (73_248_077_421_585, 100_000_000_000_000),
                ],
            )?,
            // Stage 2: the RLB high-pass. Its numerator is exactly 1, -2, 1.
            highpass: Biquad::new(
                [(1, 1), (-2, 1), (1, 1)],
                [
                    (-199_004_745_483_398, 100_000_000_000_000),
                    (99_007_225_036_621, 100_000_000_000_000),
                ],
            )?,
        })
    }

    /// One sample through both sections.
    fn step(&mut self, input: i128) -> Result<i128> {
        let shelved = self.shelf.step(input)?;
        self.highpass.step(shelved)
    }
}

/// The loudness of a whole programme, gated as BS.1770 requires.
///
/// `None` when nothing survives the gate, which is the honest answer for
/// silence: the loudness of nothing is not a number on this scale, and
/// returning some very negative value would let it be averaged with real
/// measurements.
///
/// # Errors
///
/// [`AudioStatus::UnsupportedSampleRate`] for anything but 48 kHz,
/// [`AudioStatus::ChannelCountUnsupported`] past [`MAX_WEIGHTED_CHANNELS`], and
/// [`AudioStatus::OutOfMemory`].
pub fn integrated(buffer: &AudioBuffer) -> Result<Option<Fixed>> {
    let blocks = block_powers(buffer)?;
    if blocks.is_empty() {
        return Ok(None);
    }

    // The absolute gate first: anything below it is silence for this purpose.
    let absolute = Fixed::from_rational(Rational::new(ABSOLUTE_GATE, 1)?)?;
    let mut kept = Vec::new();
    kept.try_reserve(blocks.len())
        .map_err(|_| AudioStatus::OutOfMemory)?;
    for power in &blocks {
        if loudness_of(*power)?.is_some_and(|level| level.raw() >= absolute.raw()) {
            kept.push(*power);
        }
    }
    if kept.is_empty() {
        return Ok(None);
    }

    // Then the relative gate, set ten units below the mean of what is left.
    let Some(ungated) = loudness_of(mean(&kept)?)? else {
        return Ok(None);
    };
    let threshold = ungated.checked_add(Fixed::from_rational(Rational::new(RELATIVE_GATE, 1)?)?)?;
    let mut surviving = Vec::new();
    surviving
        .try_reserve(kept.len())
        .map_err(|_| AudioStatus::OutOfMemory)?;
    for power in &kept {
        if loudness_of(*power)?.is_some_and(|level| level.raw() >= threshold.raw()) {
            surviving.push(*power);
        }
    }
    if surviving.is_empty() {
        return Ok(None);
    }
    loudness_of(mean(&surviving)?)
}

/// The loudness of one 400-millisecond window, ungated.
///
/// What a momentary meter shows. `None` for a window with no energy at all.
///
/// # Errors
///
/// As [`integrated`], and [`AudioStatus::BufferTooShort`] for a buffer
/// shorter than one block — which is not a quiet window but no window at all,
/// and is a different answer from `None`.
pub fn momentary(buffer: &AudioBuffer) -> Result<Option<Fixed>> {
    let blocks = block_powers(buffer)?;
    let Some(first) = blocks.first() else {
        return Err(AudioStatus::BufferTooShort);
    };
    loudness_of(*first)
}

/// The weighted power of every gating block in a buffer.
fn block_powers(buffer: &AudioBuffer) -> Result<Vec<i128>> {
    if buffer.rate() != SampleRate::Hz48000 {
        // BS.1770 gives its coefficients for 48 kHz and says to re-derive the
        // filter at other rates. Reusing these would measure the wrong thing
        // quietly, which is worse than refusing (R-1.3).
        return Err(AudioStatus::UnsupportedSampleRate);
    }
    if buffer.channel_count() > MAX_WEIGHTED_CHANNELS {
        return Err(AudioStatus::ChannelCountUnsupported);
    }

    let length = buffer.len();
    let mut powers = Vec::new();
    if length < BLOCK_SAMPLES {
        return Ok(powers);
    }

    // The weighted energy of each *step*, summed over every channel. Blocks
    // overlap by three quarters, so a block is four consecutive steps — which
    // means the whole measurement needs one number per step rather than one
    // per sample.
    //
    // That is not a micro-optimisation. Holding the filtered signal instead
    // would cost sixteen bytes a sample per channel, which at this crate's own
    // buffer bound is a third of a gigabyte for a stereo programme, for a
    // measurement whose answer is a few hundred numbers.
    //
    // The channels accumulate into one array rather than into one each. Every
    // channel weighs the same, and integer addition is associative, so the
    // total is identical to summing them separately — it is the same numbers
    // added in a different order.
    let steps = length / BLOCK_STEP;
    let mut energy = Vec::new();
    energy
        .try_reserve(steps)
        .map_err(|_| AudioStatus::OutOfMemory)?;
    energy.resize(steps, 0_i128);

    for channel in 0..buffer.channel_count() {
        let samples = buffer.channel(channel)?;
        // One filter per channel, run over the whole channel without
        // restarting: a biquad has state, and restarting it at each block
        // would put a transient at every block boundary — most of them inside
        // the overlap of two others.
        let mut filter = Weighting::new()?;
        for (index, sample) in samples.iter().enumerate() {
            let weighted = filter.step(normalise(*sample))?;
            let step = index / BLOCK_STEP;
            if let Some(slot) = energy.get_mut(step) {
                *slot = slot
                    .checked_add(wide_mul(weighted, weighted)?)
                    .ok_or(AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
            }
            // Samples past the last whole step belong to no complete block,
            // so their energy goes nowhere. They are still filtered, because
            // stopping early would be a special case earning nothing: they
            // are the last samples in the channel, so no later sample depends
            // on the filter state they leave behind.
        }
    }

    // A block is a window of consecutive steps, so the blocks are the sliding
    // windows of the step array and there is no index arithmetic to get wrong:
    // `windows` yields one per starting position and stops of its own accord.
    // Written as `for index in 0..count` it would need a bound check inside
    // the loop that no input can reach, which is a branch no test can cover.
    powers
        .try_reserve(energy.len().saturating_sub(STEPS_PER_BLOCK - 1))
        .map_err(|_| AudioStatus::OutOfMemory)?;
    let divisor = i128::try_from(BLOCK_SAMPLES)
        .map_err(|_| AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
    for window in energy.windows(STEPS_PER_BLOCK) {
        let mut total = 0_i128;
        for step in window {
            total = total
                .checked_add(*step)
                .ok_or(AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
        }
        powers.push(total / divisor);
    }
    Ok(powers)
}

/// The mean of a set of block powers.
fn mean(powers: &[i128]) -> Result<i128> {
    let mut total = 0_i128;
    for power in powers {
        total = total
            .checked_add(*power)
            .ok_or(AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
    }
    let count = i128::try_from(powers.len())
        .map_err(|_| AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
    if count == 0 {
        return Ok(0);
    }
    Ok(total / count)
}

/// A weighted power as a level: `-0.691 + 10 log10(power)`.
fn loudness_of(power: i128) -> Result<Option<Fixed>> {
    if power <= 0 {
        return Ok(None);
    }
    let narrowed = Fixed::from_raw(narrow_to_fixed(power)?);
    if narrowed.raw() <= 0 {
        // Below what a thirty-two bit fraction can hold is below −190 dB,
        // which is further down than any gate reaches.
        return Ok(None);
    }
    // 10 log10(x) = 10 log2(x) / log2(10).
    let log2_of_ten = Fixed::from_rational(Rational::new(3_321_928_095, 1_000_000_000)?)?;
    let decibels = narrowed
        .log2()?
        .checked_mul(Fixed::from_integer(10)?)?
        .checked_div(log2_of_ten)?;
    Ok(Some(decibels.checked_add(offset()?)?))
}

/// The −0.691 the standard adds, so that a 1 kHz tone reads at its own level.
fn offset() -> Result<Fixed> {
    Ok(Fixed::from_rational(Rational::new(-691, 1000)?)?)
}

/// A sample as a fraction of full scale, at wide precision.
///
/// Divided by 2^23 rather than by full scale, so that the most negative sample
/// is exactly −1 and the most positive is a hair under it. That asymmetry is
/// two's complement's, not this function's.
fn normalise(sample: i32) -> i128 {
    (i128::from(sample) << WIDE_BITS) >> 23
}

/// An exact decimal at wide precision.
fn wide_of(numerator: i64, denominator: i64) -> Result<i128> {
    i128::from(numerator)
        .checked_shl(WIDE_BITS)
        .map(|scaled| scaled / i128::from(denominator))
        .ok_or(AudioStatus::Time(media_editor_core::CoreStatus::Overflow))
}

/// A wide product, rounded to nearest.
fn wide_mul(left: i128, right: i128) -> Result<i128> {
    let product = left
        .checked_mul(right)
        .ok_or(AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
    let half = 1_i128 << (WIDE_BITS - 1);
    Ok(if product >= 0 {
        (product + half) >> WIDE_BITS
    } else {
        -((-product + half) >> WIDE_BITS)
    })
}

/// A wide value at the fraction a [`Fixed`] carries.
fn narrow_to_fixed(value: i128) -> Result<i64> {
    let shift = WIDE_BITS - media_editor_core::FRACTION_BITS;
    let half = 1_i128 << (shift - 1);
    let rounded = if value >= 0 {
        (value + half) >> shift
    } else {
        -((-value + half) >> shift)
    };
    i64::try_from(rounded).map_err(|_| AudioStatus::Time(media_editor_core::CoreStatus::Overflow))
}
