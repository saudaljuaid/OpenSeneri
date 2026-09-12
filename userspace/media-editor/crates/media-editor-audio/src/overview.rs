// SPDX-License-Identifier: GPL-3.0-only
//! Multiresolution waveform summaries for the timeline.
//!
//! Each bucket stores the lowest sample, highest sample, and mean square. Each
//! higher level combines two lower-level buckets, preserving peaks at every
//! zoom level. Mean-square sums use [`i128`] so rounding does not compound as
//! the pyramid is built.

use alloc::vec::Vec;

use media_editor_core::{Digest, Sha256};

use crate::buffer::{AudioBuffer, SampleRate};
use crate::status::{AudioStatus, Result};

/// The smallest block a summary may be built over.
///
/// Sixty-four samples is a little over a millisecond at 48 kHz, which is finer
/// than any drawing needs and already a sixty-fourth of the samples. Below it
/// the summary approaches the size of the thing it summarises.
pub const MIN_BUCKET: usize = 64;

/// The largest block a summary may be built over.
///
/// Level zero is the finest detail the summary can ever show, so a coarse base
/// throws away resolution that no zoom can recover. Sixteen thousand samples
/// is a third of a second: past that, the closest zoom would be blockier than
/// an edit point.
pub const MAX_BUCKET: usize = 16_384;

/// How many blocks of one level make a block of the next.
///
/// Two. Halving the count each time is what makes the pyramid's total size a
/// geometric series rather than a multiple of the sample count.
pub const FANOUT: usize = 2;

/// What one block of samples looked like.
///
/// The lowest and highest samples are real samples — some sample in the block
/// had exactly that value. The mean square is the block's energy, floored.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Bucket {
    /// The lowest sample in the block.
    minimum: i32,
    /// The highest sample in the block.
    maximum: i32,
    /// The mean of the squares of the block's samples, rounded down.
    mean_square: i64,
}

impl Bucket {
    /// Wrap three numbers that have already been computed.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::BucketNotOrdered`] if the lowest sample is above the
    /// highest, which describes no block of samples, and
    /// [`AudioStatus::BucketNotPossible`] for a negative mean square or one
    /// past the square of full scale — a block cannot hold energy it has no
    /// samples to carry.
    pub const fn new(minimum: i32, maximum: i32, mean_square: i64) -> Result<Self> {
        if minimum > maximum {
            return Err(AudioStatus::BucketNotOrdered);
        }
        if mean_square < 0 || mean_square > MAX_MEAN_SQUARE {
            return Err(AudioStatus::BucketNotPossible);
        }
        Ok(Self {
            minimum,
            maximum,
            mean_square,
        })
    }

    /// The lowest sample in the block.
    #[must_use]
    pub const fn minimum(self) -> i32 {
        self.minimum
    }

    /// The highest sample in the block.
    #[must_use]
    pub const fn maximum(self) -> i32 {
        self.maximum
    }

    /// The mean of the squares of the block's samples, rounded down.
    #[must_use]
    pub const fn mean_square(self) -> i64 {
        self.mean_square
    }

    /// How far the block reached from the centre line, either way.
    ///
    /// Saturating, because the magnitude of the most negative sample is one
    /// greater than full scale — the same reason [`AudioBuffer::peaks`]
    /// saturates.
    #[must_use]
    pub const fn peak(self) -> i32 {
        let low = self.minimum.saturating_abs();
        let high = self.maximum.saturating_abs();
        if low > high { low } else { high }
    }

    /// Whether the block holds nothing but silence.
    #[must_use]
    pub const fn is_silent(self) -> bool {
        self.minimum == 0 && self.maximum == 0
    }
}

/// The largest mean square a block of 24-bit samples can hold.
///
/// The square of the most negative sample, which is the larger of the two
/// ends.
const MAX_MEAN_SQUARE: i64 =
    (crate::buffer::NEGATIVE_FULL_SCALE as i64) * (crate::buffer::NEGATIVE_FULL_SCALE as i64);

/// A summary of a buffer at every zoom level.
///
/// Built by [`Overview::of`], read by whatever draws.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Overview {
    /// The rate the summarised samples were counted at.
    rate: SampleRate,
    /// How many channels were summarised.
    channels: usize,
    /// How many samples each channel held.
    length: usize,
    /// How many samples one block of level zero covers.
    base: usize,
    /// What was summarised, so a stale summary can be recognised as one.
    source: Digest,
    /// Level zero first, each level half the count of the one before it.
    ///
    /// Within a level the blocks are grouped by channel: all of channel
    /// nought's, then all of channel one's.
    levels: Vec<Vec<Bucket>>,
    /// This summary's own digest.
    digest: Digest,
}

impl Overview {
    /// Summarise a buffer.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::BucketSizeUnsupported`] unless `base` is a power of two
    /// between [`MIN_BUCKET`] and [`MAX_BUCKET`] — a base that is not a power
    /// of two would make the levels stop tiling each other, which is the whole
    /// structure. [`AudioStatus::BufferTooShort`] for a buffer with no
    /// samples: a summary of nothing is a thing to index into by mistake, and
    /// there is nothing to draw. And [`AudioStatus::OutOfMemory`].
    pub fn of(buffer: &AudioBuffer, base: usize) -> Result<Self> {
        check_base(base)?;
        let length = buffer.len();
        if length == 0 {
            return Err(AudioStatus::BufferTooShort);
        }
        let channels = buffer.channel_count();

        // Level zero, read from the samples. The sums are kept beside the
        // blocks in i128 and folded upward from there, so that a coarse
        // block's mean square is one division of an exact sum rather than a
        // mean of means: the error stays under one whatever the zoom.
        let mut sums = level_zero_sums(buffer, base)?;
        let mut counts = level_zero_counts(length, base, channels)?;
        let mut levels = Vec::new();
        loop {
            let mut level = Vec::new();
            level
                .try_reserve(sums.len())
                .map_err(|_| AudioStatus::OutOfMemory)?;
            for (running, count) in sums.iter().zip(counts.iter()) {
                level.push(narrow(running, *count)?);
            }
            let per_channel = level.len() / channels;
            levels
                .try_reserve(1)
                .map_err(|_| AudioStatus::OutOfMemory)?;
            levels.push(level);
            if per_channel <= 1 {
                break;
            }
            (sums, counts) = fold(&sums, &counts, channels)?;
        }

        let digest = digest_of(
            buffer.rate(),
            channels,
            length,
            base,
            buffer.digest(),
            &levels,
        );
        Ok(Self {
            rate: buffer.rate(),
            channels,
            length,
            base,
            source: buffer.digest(),
            levels,
            digest,
        })
    }

    /// Rebuild a summary from parts that were stored rather than computed.
    ///
    /// This is what a peak file is read back into. Everything it accepts is
    /// checked, because a file is somebody else's bytes (R-11.1): the levels
    /// must be the shape the base and length imply, and each must be exactly
    /// half the one before it.
    ///
    /// # Errors
    ///
    /// As [`Overview::of`] for the base and the length, and
    /// [`AudioStatus::OverviewNotShaped`] if the levels are not the pyramid
    /// those two describe.
    pub fn assemble(
        rate: SampleRate,
        channels: usize,
        length: usize,
        base: usize,
        source: Digest,
        levels: Vec<Vec<Bucket>>,
    ) -> Result<Self> {
        check_base(base)?;
        if length == 0 {
            return Err(AudioStatus::BufferTooShort);
        }
        if channels == 0 || channels > crate::buffer::MAX_CHANNELS {
            return Err(AudioStatus::ChannelCountUnsupported);
        }
        if length > crate::buffer::MAX_SAMPLES {
            return Err(AudioStatus::BufferTooLong);
        }

        let mut expected = length.div_ceil(base);
        for (index, level) in levels.iter().enumerate() {
            if level.len() != expected * channels {
                return Err(AudioStatus::OverviewNotShaped);
            }
            let last = index + 1 == levels.len();
            if last != (expected <= 1) {
                // The pyramid ends when one block covers a whole channel, and
                // not before or after. A file that stopped early would draw a
                // hole when zoomed out; one that went on would be describing
                // blocks with no samples in them.
                return Err(AudioStatus::OverviewNotShaped);
            }
            expected = expected.div_ceil(FANOUT);
        }
        if levels.is_empty() {
            return Err(AudioStatus::OverviewNotShaped);
        }

        let digest = digest_of(rate, channels, length, base, source, &levels);
        Ok(Self {
            rate,
            channels,
            length,
            base,
            source,
            levels,
            digest,
        })
    }

    /// The rate of the samples this summarises.
    #[must_use]
    pub const fn rate(&self) -> SampleRate {
        self.rate
    }

    /// How many channels this summarises.
    #[must_use]
    pub const fn channel_count(&self) -> usize {
        self.channels
    }

    /// How many samples each channel held.
    #[must_use]
    pub const fn len(&self) -> usize {
        self.length
    }

    /// Whether there is nothing here.
    ///
    /// Never true: [`Overview::of`] refuses an empty buffer. It exists because
    /// a type with `len` and no `is_empty` is a type people call `len() == 0`
    /// on, and this way the answer is in one place.
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.length == 0
    }

    /// How many samples one block of level zero covers.
    #[must_use]
    pub const fn base(&self) -> usize {
        self.base
    }

    /// The digest of the buffer this summarises.
    ///
    /// A summary beside a file that no longer hashes to this is stale, and the
    /// point of storing it is that the staleness can be *seen* rather than
    /// guessed at from a modification time.
    #[must_use]
    pub const fn source(&self) -> Digest {
        self.source
    }

    /// This summary's own digest.
    #[must_use]
    pub const fn digest(&self) -> Digest {
        self.digest
    }

    /// How many zoom levels there are.
    #[must_use]
    pub fn level_count(&self) -> usize {
        self.levels.len()
    }

    /// How many samples one block of a level covers.
    ///
    /// The shift cannot overflow, and the reason is the pyramid's own shape
    /// rather than a bound written down somewhere: levels stop when one block
    /// covers a whole channel, so the widest block is under twice the sample
    /// count, and the sample count is under [`crate::buffer::MAX_SAMPLES`].
    /// `a_width_cannot_run_off_the_end_of_a_usize` pins that, so raising
    /// either bound past what a `usize` holds fails a test rather than
    /// wrapping a block width to nought.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::NoSuchLevel`].
    pub fn samples_per_bucket(&self, level: usize) -> Result<usize> {
        if level >= self.levels.len() {
            return Err(AudioStatus::NoSuchLevel);
        }
        Ok(self.base << level)
    }

    /// One channel's blocks at one level.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::NoSuchLevel`] and
    /// [`AudioStatus::ChannelCountUnsupported`].
    pub fn buckets(&self, level: usize, channel: usize) -> Result<&[Bucket]> {
        let held = self.levels.get(level).ok_or(AudioStatus::NoSuchLevel)?;
        if channel >= self.channels {
            return Err(AudioStatus::ChannelCountUnsupported);
        }
        let width = held.len() / self.channels;
        held.get(channel * width..(channel + 1) * width)
            .ok_or(AudioStatus::OverviewNotShaped)
    }

    /// The finest level whose blocks are no larger than `samples_per_pixel`.
    ///
    /// This is what a drawing asks for: given how many samples one pixel
    /// stands for, which level can be read a block to a pixel without reading
    /// more blocks than there are pixels. It answers with the *coarsest* level
    /// that still fits, since a finer one would mean merging in the drawing.
    ///
    /// Level zero when the request is finer than level zero — the summary
    /// cannot show detail it did not keep, and saying so by returning its best
    /// is more useful than refusing, because the caller who wants the samples
    /// themselves at that zoom can go and read them.
    #[must_use]
    pub fn level_for(&self, samples_per_pixel: usize) -> usize {
        let mut chosen = 0;
        let mut width = self.base;
        for level in 0..self.levels.len() {
            if width > samples_per_pixel {
                break;
            }
            chosen = level;
            width <<= 1;
        }
        chosen
    }

    /// Every block of one channel at one level, over a span of samples,
    /// combined into one.
    ///
    /// The lowest and highest are exact — they are the lowest and highest of
    /// real samples. The mean square is a count-weighted mean of stored means,
    /// so it is within two of the true mean square of those blocks' samples:
    /// one from each stored value's own floor, one from this one.
    ///
    /// The span is in samples and is taken to the blocks that overlap it,
    /// which is what a drawing wants: a pixel showing samples 100 to 300 must
    /// show the block those samples are in, not nothing.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::NoSuchLevel`],
    /// [`AudioStatus::ChannelCountUnsupported`], and
    /// [`AudioStatus::EmptyWindow`] if the span touches no block at all.
    pub fn window(&self, level: usize, channel: usize, start: usize, end: usize) -> Result<Bucket> {
        let width = self.samples_per_bucket(level)?;
        let buckets = self.buckets(level, channel)?;
        if end <= start {
            return Err(AudioStatus::EmptyWindow);
        }
        let first = start / width;
        let last = (end - 1) / width;
        let held = buckets
            .get(first..=last.min(buckets.len().saturating_sub(1)))
            .filter(|slice| !slice.is_empty())
            .ok_or(AudioStatus::EmptyWindow)?;

        let mut minimum = i32::MAX;
        let mut maximum = i32::MIN;
        let mut weighted = 0_i128;
        let mut total = 0_i128;
        for (offset, bucket) in held.iter().enumerate() {
            minimum = minimum.min(bucket.minimum);
            maximum = maximum.max(bucket.maximum);
            let count = i128::try_from(self.samples_in(width, first + offset))
                .map_err(|_| AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
            weighted = weighted
                .checked_add(i128::from(bucket.mean_square) * count)
                .ok_or(AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
            total = total
                .checked_add(count)
                .ok_or(AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
        }
        if total == 0 {
            return Err(AudioStatus::EmptyWindow);
        }
        let mean = i64::try_from(weighted / total)
            .map_err(|_| AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
        Bucket::new(minimum, maximum, mean)
    }

    /// How many samples a particular block actually covers.
    ///
    /// Every block but the last covers a full width. The last covers what is
    /// left, which is why it is not simply the width: a mean over the width
    /// when only a few samples are there would divide real energy by imaginary
    /// silence and draw the end of every clip quieter than it is.
    ///
    /// A block past the end covers nothing, which is the honest answer and
    /// leaves [`Overview::window`] to refuse a span made entirely of them.
    /// `index * width` cannot overflow for the same reason the width itself
    /// cannot: their product is the block's start, which is under the sample
    /// count.
    fn samples_in(&self, width: usize, index: usize) -> usize {
        self.length.saturating_sub(index * width).min(width)
    }
}

/// Whether a block size is one a summary can be built on.
///
/// A power of two, because the levels tile each other by halving and a base
/// that is not one would leave a level whose blocks straddle two of the level
/// below. Within the useful range, because a summary finer than [`MIN_BUCKET`]
/// approaches the size of what it summarises, and one coarser than
/// [`MAX_BUCKET`] cannot show detail that no zoom can then recover.
///
/// Public because a reader of a stored summary needs the same answer before it
/// can work out how many blocks to expect, and a second copy of this rule
/// living in whatever crate reads files is a rule with two statements and
/// therefore a rule that will one day disagree with itself.
///
/// # Errors
///
/// [`AudioStatus::BucketSizeUnsupported`].
pub const fn check_base(base: usize) -> Result<()> {
    if base.is_power_of_two() && base >= MIN_BUCKET && base <= MAX_BUCKET {
        Ok(())
    } else {
        Err(AudioStatus::BucketSizeUnsupported)
    }
}

/// The exact sum of squares of every level-zero block, channel by channel.
fn level_zero_sums(buffer: &AudioBuffer, base: usize) -> Result<Vec<Running>> {
    let count = buffer.len().div_ceil(base);
    let mut sums = Vec::new();
    sums.try_reserve(count * buffer.channel_count())
        .map_err(|_| AudioStatus::OutOfMemory)?;
    for channel in 0..buffer.channel_count() {
        let samples = buffer.channel(channel)?;
        for block in samples.chunks(base) {
            let mut running = Running::EMPTY;
            for sample in block {
                running.take(*sample)?;
            }
            sums.push(running);
        }
    }
    Ok(sums)
}

/// How many samples each level-zero block covers, channel by channel.
fn level_zero_counts(length: usize, base: usize, channels: usize) -> Result<Vec<usize>> {
    let count = length.div_ceil(base);
    let mut counts = Vec::new();
    counts
        .try_reserve(count * channels)
        .map_err(|_| AudioStatus::OutOfMemory)?;
    for _ in 0..channels {
        for index in 0..count {
            counts.push(base.min(length - index * base));
        }
    }
    Ok(counts)
}

/// One level's sums and counts, folded into the next level up.
///
/// Pairs within a channel, never across one: the last block of channel nought
/// and the first of channel one are not adjacent in time, and summarising them
/// together would draw one channel's ending into the other's beginning.
fn fold(sums: &[Running], counts: &[usize], channels: usize) -> Result<(Vec<Running>, Vec<usize>)> {
    let width = sums.len() / channels;
    let next = width.div_ceil(FANOUT);
    let mut folded = Vec::new();
    folded
        .try_reserve(next * channels)
        .map_err(|_| AudioStatus::OutOfMemory)?;
    let mut lengths = Vec::new();
    lengths
        .try_reserve(next * channels)
        .map_err(|_| AudioStatus::OutOfMemory)?;
    for channel in 0..channels {
        for index in 0..next {
            let mut running = Running::EMPTY;
            let mut count = 0;
            for step in 0..FANOUT {
                let at = index * FANOUT + step;
                if at >= width {
                    break;
                }
                running.join(&sums[channel * width + at])?;
                count += counts[channel * width + at];
            }
            folded.push(running);
            lengths.push(count);
        }
    }
    Ok((folded, lengths))
}

/// A block's lowest and highest sample and the exact sum of its squares.
#[derive(Clone, Copy, Debug)]
struct Running {
    /// The lowest sample seen, or [`i32::MAX`] if none has been.
    minimum: i32,
    /// The highest sample seen, or [`i32::MIN`] if none has been.
    maximum: i32,
    /// The sum of the squares of every sample seen, exactly.
    ///
    /// At the buffer's own bound this reaches about two to the seventieth, so
    /// it is not an [`i64`] and the width is not a precaution.
    sum: i128,
}

impl Running {
    /// Nothing seen yet.
    const EMPTY: Self = Self {
        minimum: i32::MAX,
        maximum: i32::MIN,
        sum: 0,
    };

    /// Account for one sample.
    fn take(&mut self, sample: i32) -> Result<()> {
        self.minimum = self.minimum.min(sample);
        self.maximum = self.maximum.max(sample);
        let square = i128::from(sample) * i128::from(sample);
        self.sum = self
            .sum
            .checked_add(square)
            .ok_or(AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
        Ok(())
    }

    /// Account for everything another block saw.
    fn join(&mut self, other: &Self) -> Result<()> {
        self.minimum = self.minimum.min(other.minimum);
        self.maximum = self.maximum.max(other.maximum);
        self.sum = self
            .sum
            .checked_add(other.sum)
            .ok_or(AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
        Ok(())
    }
}

/// Turn an exact running sum into a storable block.
fn narrow(running: &Running, count: usize) -> Result<Bucket> {
    if count == 0 {
        return Err(AudioStatus::OverviewNotShaped);
    }
    let divisor = i128::try_from(count)
        .map_err(|_| AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
    let mean = i64::try_from(running.sum / divisor)
        .map_err(|_| AudioStatus::Time(media_editor_core::CoreStatus::Overflow))?;
    Bucket::new(running.minimum, running.maximum, mean)
}

/// The digest of everything a summary is.
fn digest_of(
    rate: SampleRate,
    channels: usize,
    length: usize,
    base: usize,
    source: Digest,
    levels: &[Vec<Bucket>],
) -> Digest {
    let mut hasher = Sha256::new();
    hasher.update(&[rate.tag()]);
    hasher.update(&u32::try_from(channels).unwrap_or(u32::MAX).to_le_bytes());
    hasher.update(&u64::try_from(length).unwrap_or(u64::MAX).to_le_bytes());
    hasher.update(&u64::try_from(base).unwrap_or(u64::MAX).to_le_bytes());
    hasher.update(source.bytes());
    hasher.update(
        &u32::try_from(levels.len())
            .unwrap_or(u32::MAX)
            .to_le_bytes(),
    );
    for level in levels {
        hasher.update(&u64::try_from(level.len()).unwrap_or(u64::MAX).to_le_bytes());
        for bucket in level {
            hasher.update(&bucket.minimum.to_le_bytes());
            hasher.update(&bucket.maximum.to_le_bytes());
            hasher.update(&bucket.mean_square.to_le_bytes());
        }
    }
    hasher.finish()
}
