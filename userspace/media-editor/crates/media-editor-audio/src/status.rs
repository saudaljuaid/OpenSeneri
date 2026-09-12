// SPDX-License-Identifier: GPL-3.0-only
//! Every way the sound side refuses.

use media_editor_core::CoreStatus;

/// A refusal from the audio side.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum AudioStatus {
    /// An exact arithmetic refusal from the core types.
    Time(CoreStatus),
    /// An allocation the caller must handle.
    OutOfMemory,
    /// A gain outside the range a fader offers.
    GainOutOfRange,
    /// Silence cannot be undone: nothing multiplied by zero comes back.
    NoInverse,
    /// A pan position outside hard left to hard right.
    PanOutOfRange,
    /// A buffer with no channels, or more than the layout allows.
    ChannelCountUnsupported,
    /// Buffers of different lengths, rates or channel counts cannot be mixed
    /// without a decision about which is right.
    NotMixable,
    /// A sample rate this build has no meaning for.
    UnsupportedSampleRate,
    /// A buffer longer than the policy bound.
    BufferTooLong,
    /// A buffer shorter than the window a measurement is defined over.
    BufferTooShort,
    /// A buffer whose sample count is not a whole number of frames.
    RaggedBuffer,
    /// The mix reached full scale, so what would have been written is not what
    /// the arithmetic said.
    Clipped,
    /// A summary block size that is not a power of two, or is outside the
    /// range a summary is useful over.
    BucketSizeUnsupported,
    /// A summary block whose lowest sample is above its highest, which
    /// describes no block of samples.
    BucketNotOrdered,
    /// A summary block holding energy no samples could carry.
    BucketNotPossible,
    /// A summary whose levels are not the pyramid its base and length
    /// describe.
    OverviewNotShaped,
    /// A zoom level a summary does not hold.
    NoSuchLevel,
    /// A span of samples that touches no summary block.
    EmptyWindow,
}

impl AudioStatus {
    /// One line naming the condition.
    #[must_use]
    pub const fn describe(self) -> &'static str {
        match self {
            Self::Time(status) => status.describe(),
            Self::OutOfMemory => "the allocation could not be satisfied",
            Self::GainOutOfRange => "that gain is past what a fader offers",
            Self::NoInverse => "silence cannot be undone",
            Self::PanOutOfRange => "a pan position runs from hard left to hard right",
            Self::ChannelCountUnsupported => "that channel count is not one this build lays out",
            Self::NotMixable => "these buffers do not describe the same span of sound",
            Self::UnsupportedSampleRate => "that sample rate is not one this build carries",
            Self::BufferTooLong => "the buffer is past the policy bound",
            Self::BufferTooShort => "the buffer is shorter than the window this measures over",
            Self::RaggedBuffer => "the samples are not a whole number of frames",
            Self::Clipped => "the mix reached full scale and could not be written as computed",
            Self::BucketSizeUnsupported => {
                "a summary block is a power of two samples within the useful range"
            }
            Self::BucketNotOrdered => "that block's lowest sample is above its highest",
            Self::BucketNotPossible => "that block holds energy no samples could carry",
            Self::OverviewNotShaped => "the summary's levels are not the pyramid it describes",
            Self::NoSuchLevel => "the summary does not hold that zoom level",
            Self::EmptyWindow => "that span of samples touches no summary block",
        }
    }
}

impl core::fmt::Display for AudioStatus {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        formatter.write_str(self.describe())
    }
}

/// The result of any fallible operation in this crate.
pub type Result<T> = core::result::Result<T, AudioStatus>;
