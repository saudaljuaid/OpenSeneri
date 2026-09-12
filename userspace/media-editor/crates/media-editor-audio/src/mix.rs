// SPDX-License-Identifier: GPL-3.0-only
//! The mixer: adding sound together, and saying so when it will not fit.
//!
//! Summing is the easy half. The hard half is the headroom question, and it is
//! the one that decides whether a mixer is usable.
//!
//! Two full-scale sources summed are twice full scale. Something has to give,
//! and there are exactly three honest options: refuse, clip, or report. This
//! mixer accumulates in a width that cannot overflow, then **tells the caller**
//! whether the result reached full scale — [`mix`] returns the mix and a
//! [`MixReport`] alongside it, rather than either clipping in silence or
//! refusing a session that a fader move would fix.
//!
//! Silence is not a special case in the arithmetic and is a special case in
//! the meaning: adding a silent source to a mix must leave it *bit for bit*
//! unchanged, and adding a source to silence must give that source back
//! exactly. Both are asserted, because a mixer that drifts by a bit when a
//! muted channel is present is a mixer whose bounces are not reproducible.
//!
//! Every sum is integer arithmetic in `i64`, so it is exact and it is
//! associative and commutative — a bus does not depend on the order its
//! channels arrive in, which is what lets a scheduler ever run them in
//! parallel (R-6.2).

use alloc::vec::Vec;

use media_editor_core::Fixed;

use crate::buffer::{AudioBuffer, FULL_SCALE, NEGATIVE_FULL_SCALE};
use crate::gain::Gain;
use crate::pan::Pan;
use crate::status::{AudioStatus, Result};

/// What happened to the headroom.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct MixReport {
    /// How many samples were written at a value the arithmetic did not give,
    /// because the sum was past full scale.
    pub clipped: u64,
    /// How far past full scale the loudest sample went, as a magnitude in
    /// sample units. Zero when nothing clipped.
    pub overshoot: i64,
}

impl MixReport {
    /// Whether anything was written differently from how it was computed.
    #[must_use]
    pub const fn is_clean(self) -> bool {
        self.clipped == 0
    }
}

/// One source going into a mix.
#[derive(Clone, Debug)]
pub struct Source<'a> {
    buffer: &'a AudioBuffer,
    gain: Gain,
    /// Where the gain has arrived by the sample *after* the last one, if it is
    /// moving. [`None`] is a fader that is not being moved at all, which is
    /// not the same as one moving from a value to itself: the first is a
    /// copy, the second is a multiply by one at every sample.
    arriving: Option<Gain>,
}

impl<'a> Source<'a> {
    /// A source at a gain that does not move.
    #[must_use]
    pub const fn new(buffer: &'a AudioBuffer, gain: Gain) -> Self {
        Self {
            buffer,
            gain,
            arriving: None,
        }
    }

    /// A source whose fader moves across the buffer.
    ///
    /// `from` is the gain at the first sample. `to` is the gain at the sample
    /// *after* the last one, which no sample in this buffer receives — the
    /// next block's first sample does.
    ///
    /// That half-open shape is the same one the rest of the project uses for
    /// spans: a frame's samples run up to the next frame's first, an EDL's out
    /// point is exclusive, and a summary block ends where the next begins.
    /// Here it is what makes consecutive blocks *tile* a fader move: block
    /// `n` ends one step short of `to`, block `n + 1` starts exactly at it,
    /// and no sample is given a gain twice or skipped.
    ///
    /// Closing the interval instead — arriving at `to` on the last sample —
    /// would repeat one gain at every block boundary. On a fast move that
    /// repetition is a step, and a step at a regular interval is a tone: it
    /// would put a hum at the frame rate into every automated fade.
    #[must_use]
    pub const fn ramped(buffer: &'a AudioBuffer, from: Gain, to: Gain) -> Self {
        Self {
            buffer,
            gain: from,
            arriving: Some(to),
        }
    }

    /// The two factors a moving fader runs between, or [`None`] if it is not
    /// moving.
    ///
    /// Computed once for the whole buffer rather than at each sample, because
    /// a factor is `10^(dB/20)` and that is a logarithm and an exponential —
    /// not something to run forty-eight thousand times a second for a number
    /// that only takes two values.
    fn travel(&self) -> Result<Option<(Fixed, Fixed)>> {
        let Some(to) = self.arriving else {
            return Ok(None);
        };
        Ok(Some((self.gain.factor()?, to.factor()?)))
    }
}

/// A fader's position part-way through a block.
///
/// Interpolated in the *factor* rather than in decibels, which is a choice
/// and is bounded. A fader's travel is logarithmic, so the true path between
/// two positions is the geometric mean and this takes the arithmetic one —
/// for a six-decibel move the two differ by half a decibel at the middle of
/// the block and by nothing at either end.
///
/// The ends are what matter. They are exact, so consecutive blocks join with
/// no step whatever happens in between, and a block is one frame: the
/// approximation lives inside a fortieth of a second and shrinks as the
/// blocks do. Interpolating in decibels instead would mean a logarithm and an
/// exponential at every sample, at every rate, for a difference no listener
/// meets.
///
/// The division truncates, and here that is genuinely beneath notice rather
/// than a rule left unstated: a raw unit of the factor is `2^-32`, and a
/// 24-bit sample multiplied by that moves by less than `2^-9` of one sample
/// unit. No rounding rule available at this point can change an output
/// sample, so there is nothing for a test to pin and nothing to get wrong.
fn between(from: Fixed, to: Fixed, index: usize, length: usize) -> Result<Fixed> {
    let overflow = || AudioStatus::Time(media_editor_core::CoreStatus::Overflow);
    let span = i128::from(to.raw()) - i128::from(from.raw());
    let index = i128::try_from(index).map_err(|_| overflow())?;
    let length = i128::try_from(length).map_err(|_| overflow())?;
    let step = span
        .checked_mul(index)
        .ok_or_else(overflow)?
        .checked_div(length)
        .ok_or_else(overflow)?;
    let raw = i128::from(from.raw())
        .checked_add(step)
        .ok_or_else(overflow)?;
    Ok(Fixed::from_raw(i64::try_from(raw).map_err(|_| overflow())?))
}

/// Add sources together, channel for channel.
///
/// Every source must describe the same span of sound — the same rate, the same
/// channel count, the same length. Two buffers at different rates are not two
/// things to add; making them addable is resampling, which is a filter and a
/// decision with a name (R-1.3).
///
/// # Errors
///
/// [`AudioStatus::NotMixable`] for sources that do not agree,
/// [`AudioStatus::ChannelCountUnsupported`] for an empty source list, and
/// [`AudioStatus::OutOfMemory`] if the result cannot be held.
pub fn mix(sources: &[Source<'_>]) -> Result<(AudioBuffer, MixReport)> {
    let Some(first) = sources.first() else {
        return Err(AudioStatus::ChannelCountUnsupported);
    };
    let shape = first.buffer;
    if sources.iter().any(|source| !shape.matches(source.buffer)) {
        return Err(AudioStatus::NotMixable);
    }

    let channel_count = shape.channel_count();
    let length = shape.len();
    let mut report = MixReport::default();

    // Each source's fader travel, worked out once for the whole mix rather
    // than per channel or per sample.
    let mut travels = Vec::new();
    travels
        .try_reserve(sources.len())
        .map_err(|_| AudioStatus::OutOfMemory)?;
    for source in sources {
        travels.push(source.travel()?);
    }

    let mut out = Vec::new();
    out.try_reserve(channel_count)
        .map_err(|_| AudioStatus::OutOfMemory)?;

    for channel in 0..channel_count {
        let mut written = Vec::new();
        written
            .try_reserve(length)
            .map_err(|_| AudioStatus::OutOfMemory)?;
        for index in 0..length {
            let mut sum = 0_i64;
            for (source, travel) in sources.iter().zip(travels.iter()) {
                let sample = source.buffer.channel(channel)?[index];
                let contribution = match travel {
                    // A fader that is not moving is not the same as one moving
                    // from a value to itself: this path keeps unity a copy and
                    // silence a nought, which the ramp cannot promise.
                    None => scaled(sample, source.gain)?,
                    Some((from, to)) => apply(sample, between(*from, *to, index, length)?)?,
                };
                sum = sum
                    .checked_add(contribution)
                    .ok_or(AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
            }
            written.push(clamp(sum, &mut report));
        }
        out.push(written);
    }
    Ok((AudioBuffer::new(shape.rate(), out)?, report))
}

/// Send a mono source to a stereo pair at a position and a gain.
///
/// # Errors
///
/// [`AudioStatus::ChannelCountUnsupported`] if the source is not mono, and any
/// refusal from the arithmetic.
pub fn pan_to_stereo(source: &AudioBuffer, pan: Pan, gain: Gain) -> Result<AudioBuffer> {
    if source.channel_count() != 1 {
        return Err(AudioStatus::ChannelCountUnsupported);
    }
    let (left_gain, right_gain) = pan.gains()?;
    let factor = gain.factor()?;
    let mono = source.channel(0)?;

    let mut channels = Vec::new();
    channels
        .try_reserve(2)
        .map_err(|_| AudioStatus::OutOfMemory)?;
    for side in [left_gain, right_gain] {
        let combined = side.checked_mul(factor)?;
        let mut written = Vec::new();
        written
            .try_reserve(mono.len())
            .map_err(|_| AudioStatus::OutOfMemory)?;
        let mut ignored = MixReport::default();
        for sample in mono {
            written.push(clamp(apply(*sample, combined)?, &mut ignored));
        }
        channels.push(written);
    }
    AudioBuffer::new(source.rate(), channels)
}

/// Apply a gain to one sample, exactly.
fn scaled(sample: i32, gain: Gain) -> Result<i64> {
    if gain.is_silent() {
        return Ok(0);
    }
    if gain == Gain::UNITY {
        // Unity is a copy, not a multiply. Going through the fixed-point round
        // trip would be correct to the last bit anyway, and this says out loud
        // that a fader at unity does nothing at all.
        return Ok(i64::from(sample));
    }
    apply(sample, gain.factor()?)
}

/// Multiply a sample by a fixed-point factor, rounding half away from zero.
///
/// Rounding towards zero would bias every quiet passage towards silence, and
/// rounding half up would bias the whole signal upwards by half a bit — which
/// is a DC offset, and a DC offset is a click at every edit point.
fn apply(sample: i32, factor: Fixed) -> Result<i64> {
    let product = i64::from(sample)
        .checked_mul(factor.raw())
        .ok_or(AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
    let half = 1_i64 << (media_editor_core::FRACTION_BITS - 1);
    let rounded = if product >= 0 {
        product.saturating_add(half)
    } else {
        product.saturating_sub(half)
    };
    Ok(rounded / (1_i64 << media_editor_core::FRACTION_BITS))
}

/// Write a sum as a sample, recording what full scale cost.
fn clamp(sum: i64, report: &mut MixReport) -> i32 {
    let low = i64::from(NEGATIVE_FULL_SCALE);
    let high = i64::from(FULL_SCALE);
    if sum > high || sum < low {
        report.clipped = report.clipped.saturating_add(1);
        let over = if sum > high { sum - high } else { low - sum };
        report.overshoot = report.overshoot.max(over);
        return if sum > high {
            FULL_SCALE
        } else {
            NEGATIVE_FULL_SCALE
        };
    }
    i32::try_from(sum).unwrap_or(FULL_SCALE)
}
