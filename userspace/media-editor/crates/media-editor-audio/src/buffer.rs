// SPDX-License-Identifier: GPL-3.0-only
//! Sound, as a block of samples that knows what it is.
//!
//! Deinterleaved, one vector per channel, because every operation a mixer
//! performs is per channel and interleaving makes each of them stride. Samples
//! are signed 32-bit integers holding 24-bit audio, which is the width a
//! console works in: 16-bit sources fit with room above and below, the mix bus
//! has headroom to accumulate into, and nothing anywhere is a float — Phipia
//! offers no guarantee that a Ring 3 program may execute one.
//!
//! A buffer is immutable once made, for the same reason a frame is (R-8.1): it
//! can then be shared, cached by its digest, and reasoned about without asking
//! who else holds it.
//!
//! A buffer carries its sample rate, and two buffers at different rates cannot
//! be mixed. That is not a limitation to work around — resampling is a filter,
//! a filter is a decision with a name, and silently choosing one is how a
//! session ends up with aliasing nobody ordered (R-1.3).

use alloc::vec::Vec;

use media_editor_core::{CoreStatus, Digest, Rational, Sha256};

use crate::status::{AudioStatus, Result};

/// The most samples one buffer may hold, per channel.
///
/// About two minutes at 96 kHz. A block longer than this is a file rather than
/// a buffer, and the bound exists so nothing can ask for one the size of
/// memory before anything checks (R-11.2).
pub const MAX_SAMPLES: usize = 12_000_000;

/// The most channels one buffer may carry.
///
/// Sixteen: stereo, 5.1, 7.1 and a little room. Beyond that is a stem bus,
/// which is several buffers rather than one wide one.
pub const MAX_CHANNELS: usize = 16;

/// Full scale, as a sample value.
///
/// A 24-bit sample runs from `-8_388_608` to `8_388_607`. The positive side is
/// one smaller than the negative side, which is a property of two's complement
/// rather than an oversight, and it is why clipping is checked against both
/// ends rather than against a magnitude.
pub const FULL_SCALE: i32 = 8_388_607;

/// The most negative sample value.
pub const NEGATIVE_FULL_SCALE: i32 = -8_388_608;

/// A rate in samples per second.
///
/// The four a production actually meets. A rate this build does not carry is
/// refused rather than approximated, because approximating a sample rate is
/// resampling and that is somebody's decision to make.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum SampleRate {
    /// What a compact disc runs at, and what most music arrives as.
    Hz44100,
    /// What a production runs at, and what the timebase `AUDIO_48K` counts.
    Hz48000,
    /// Double 48, for material that will be pitched down.
    Hz96000,
    /// Double 44.1.
    Hz88200,
}

impl SampleRate {
    /// The timebase this rate counts in.
    ///
    /// Here rather than beside whatever needs it, because more than one thing
    /// does: the mixer decides which sample a timeline instant falls in, and
    /// the reel format decides how many samples a run of frames holds. Those
    /// two must agree exactly — a format that disagreed with the mixer by one
    /// sample would refuse every export it was handed — and the way to make
    /// two things agree is to give them one answer rather than two.
    #[must_use]
    pub const fn timebase(self) -> media_editor_core::Timebase {
        match self {
            Self::Hz44100 => media_editor_core::Timebase::AUDIO_44K1,
            Self::Hz48000 => media_editor_core::Timebase::AUDIO_48K,
            Self::Hz88200 => media_editor_core::Timebase::AUDIO_88K2,
            Self::Hz96000 => media_editor_core::Timebase::AUDIO_96K,
        }
    }

    /// The rate as a whole number of samples per second.
    #[must_use]
    pub const fn hertz(self) -> u32 {
        match self {
            Self::Hz44100 => 44_100,
            Self::Hz48000 => 48_000,
            Self::Hz88200 => 88_200,
            Self::Hz96000 => 96_000,
        }
    }

    /// The rate named by a number of samples per second.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::UnsupportedSampleRate`] for anything else.
    pub const fn from_hertz(hertz: u32) -> Result<Self> {
        match hertz {
            44_100 => Ok(Self::Hz44100),
            48_000 => Ok(Self::Hz48000),
            88_200 => Ok(Self::Hz88200),
            96_000 => Ok(Self::Hz96000),
            _ => Err(AudioStatus::UnsupportedSampleRate),
        }
    }

    /// The tag this rate is written as in a file.
    #[must_use]
    pub const fn tag(self) -> u8 {
        match self {
            Self::Hz44100 => 1,
            Self::Hz48000 => 2,
            Self::Hz88200 => 3,
            Self::Hz96000 => 4,
        }
    }

    /// The rate a stored byte names.
    ///
    /// Beside [`SampleRate::tag`] rather than in whatever crate happens to
    /// read a file, so that the two directions of one table cannot drift
    /// apart — and so that a round trip over all four is one test rather than
    /// one per format.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::UnsupportedSampleRate`] for a byte naming no rate this
    /// build carries.
    pub const fn from_tag(tag: u8) -> Result<Self> {
        match tag {
            1 => Ok(Self::Hz44100),
            2 => Ok(Self::Hz48000),
            3 => Ok(Self::Hz88200),
            4 => Ok(Self::Hz96000),
            _ => Err(AudioStatus::UnsupportedSampleRate),
        }
    }

    /// Every rate this build carries, in tag order.
    ///
    /// So that a test over all of them cannot silently miss one added later.
    pub const ALL: [Self; 4] = [Self::Hz44100, Self::Hz48000, Self::Hz88200, Self::Hz96000];
}

/// A block of sound.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AudioBuffer {
    rate: SampleRate,
    channels: Vec<Vec<i32>>,
    digest: Digest,
}

impl AudioBuffer {
    /// A buffer from one vector of samples per channel.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::ChannelCountUnsupported`] for no channels or more than
    /// [`MAX_CHANNELS`], [`AudioStatus::RaggedBuffer`] if the channels are not
    /// all the same length, and [`AudioStatus::BufferTooLong`] past
    /// [`MAX_SAMPLES`].
    pub fn new(rate: SampleRate, channels: Vec<Vec<i32>>) -> Result<Self> {
        if channels.is_empty() || channels.len() > MAX_CHANNELS {
            return Err(AudioStatus::ChannelCountUnsupported);
        }
        let length = channels[0].len();
        if length > MAX_SAMPLES {
            return Err(AudioStatus::BufferTooLong);
        }
        if channels.iter().any(|channel| channel.len() != length) {
            // A buffer whose channels are different lengths does not describe a
            // span of sound. Padding the short ones would be inventing silence
            // somebody did not record.
            return Err(AudioStatus::RaggedBuffer);
        }
        let digest = digest_of(rate, &channels);
        Ok(Self {
            rate,
            channels,
            digest,
        })
    }

    /// A buffer of silence.
    ///
    /// # Errors
    ///
    /// As [`AudioBuffer::new`], and [`AudioStatus::OutOfMemory`].
    pub fn silence(rate: SampleRate, channels: usize, samples: usize) -> Result<Self> {
        if channels == 0 || channels > MAX_CHANNELS {
            return Err(AudioStatus::ChannelCountUnsupported);
        }
        if samples > MAX_SAMPLES {
            return Err(AudioStatus::BufferTooLong);
        }
        let mut held = Vec::new();
        held.try_reserve(channels)
            .map_err(|_| AudioStatus::OutOfMemory)?;
        for _ in 0..channels {
            let mut channel = Vec::new();
            channel
                .try_reserve(samples)
                .map_err(|_| AudioStatus::OutOfMemory)?;
            channel.resize(samples, 0);
            held.push(channel);
        }
        Self::new(rate, held)
    }

    /// The rate these samples are counted at.
    #[must_use]
    pub const fn rate(&self) -> SampleRate {
        self.rate
    }

    /// How many channels.
    #[must_use]
    pub fn channel_count(&self) -> usize {
        self.channels.len()
    }

    /// How many samples each channel holds.
    #[must_use]
    pub fn len(&self) -> usize {
        self.channels[0].len()
    }

    /// Whether this buffer holds no samples at all.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// One channel's samples.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::ChannelCountUnsupported`] if there is no such channel.
    pub fn channel(&self, index: usize) -> Result<&[i32]> {
        self.channels
            .get(index)
            .map(Vec::as_slice)
            .ok_or(AudioStatus::ChannelCountUnsupported)
    }

    /// What this buffer is, independent of where it came from.
    #[must_use]
    pub const fn digest(&self) -> Digest {
        self.digest
    }

    /// Whether two buffers describe the same span of sound and so can be
    /// added.
    #[must_use]
    pub fn matches(&self, other: &Self) -> bool {
        self.rate == other.rate
            && self.channel_count() == other.channel_count()
            && self.len() == other.len()
    }

    /// The loudest sample in each channel, as a magnitude.
    ///
    /// The magnitude of the most negative sample is one greater than full
    /// scale, so this saturates rather than wrapping — a peak meter that
    /// reported minus full scale as a large negative number would be reporting
    /// the one sample a listener most needs to know about as the quietest.
    #[must_use]
    pub fn peaks(&self) -> Vec<i32> {
        self.channels
            .iter()
            .map(|channel| {
                channel
                    .iter()
                    .map(|sample| sample.saturating_abs())
                    .max()
                    .unwrap_or(0)
            })
            .collect()
    }

    /// This buffer with a fade ramped across it, exactly.
    ///
    /// `from` is the fraction at the first sample and `to` is the fraction at
    /// the sample *after* the last — the same half-open shape the fader ramp
    /// uses, and for the same reason: consecutive blocks then tile a fade
    /// rather than repeating one value at every seam, and a repetition at a
    /// regular interval is a tone.
    ///
    /// Exact integer arithmetic, rounded once, half away from zero — the same
    /// rounding the compositor uses on a coverage, so a picture fade and a
    /// sound fade of the same length agree about where they are.
    ///
    /// # Errors
    ///
    /// [`AudioStatus::OutOfMemory`], or [`AudioStatus::Time`] wrapping an
    /// overflow.
    pub fn faded(&self, from: Rational, to: Rational) -> Result<Self> {
        let count =
            i64::try_from(self.len()).map_err(|_| AudioStatus::Time(CoreStatus::Overflow))?;
        let travel = to.checked_sub(from).map_err(AudioStatus::Time)?;
        let mut channels = Vec::new();
        channels
            .try_reserve(self.channels.len())
            .map_err(|_| AudioStatus::OutOfMemory)?;
        for channel in &self.channels {
            let mut out = Vec::new();
            out.try_reserve(channel.len())
                .map_err(|_| AudioStatus::OutOfMemory)?;
            for (index, sample) in channel.iter().enumerate() {
                let along =
                    i64::try_from(index).map_err(|_| AudioStatus::Time(CoreStatus::Overflow))?;
                let fraction = if count == 0 {
                    from
                } else {
                    from.checked_add(
                        travel
                            .checked_mul(Rational::new(along, count).map_err(AudioStatus::Time)?)
                            .map_err(AudioStatus::Time)?,
                    )
                    .map_err(AudioStatus::Time)?
                };
                out.push(scaled(*sample, fraction)?);
            }
            channels.push(out);
        }
        Self::new(self.rate, channels)
    }
}

/// The digest of a buffer's rate and every sample in it.
fn digest_of(rate: SampleRate, channels: &[Vec<i32>]) -> Digest {
    let mut hasher = Sha256::new();
    hasher.update(&[rate.tag()]);
    let count = u32::try_from(channels.len()).unwrap_or(u32::MAX);
    hasher.update(&count.to_le_bytes());
    for channel in channels {
        let length = u64::try_from(channel.len()).unwrap_or(u64::MAX);
        hasher.update(&length.to_le_bytes());
        for sample in channel {
            hasher.update(&sample.to_le_bytes());
        }
    }
    hasher.finish()
}

/// A sample at a fraction of itself, rounded once, half away from zero.
fn scaled(sample: i32, fraction: Rational) -> Result<i32> {
    let numerator = i64::from(sample)
        .checked_mul(fraction.numerator())
        .ok_or(AudioStatus::Time(CoreStatus::Overflow))?;
    let denominator = fraction.denominator();
    let doubled = numerator
        .checked_mul(2)
        .ok_or(AudioStatus::Time(CoreStatus::Overflow))?;
    let rounded = if doubled >= 0 {
        (doubled + denominator) / (denominator * 2)
    } else {
        (doubled - denominator) / (denominator * 2)
    };
    i32::try_from(rounded).map_err(|_| AudioStatus::Time(CoreStatus::Overflow))
}
