// SPDX-License-Identifier: GPL-3.0-only
//! Every way a frame is refused.

use media_editor_core::CoreStatus;

/// A refusal from the media types.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum MediaStatus {
    /// An arithmetic or timebase refusal from the core types.
    Time(CoreStatus),
    /// An allocation the caller must handle.
    OutOfMemory,
    /// A dimension is zero.
    EmptyGeometry,
    /// A dimension is larger than the format allows.
    GeometryTooLarge,
    /// A subsampled format was given a dimension it cannot halve.
    OddDimension,
    /// A subsampled format was offered without a chroma siting, or an
    /// unsubsampled one was offered with one.
    SitingMismatch,
    /// A format with an alpha channel was offered without an alpha
    /// association, or one without a channel was offered with one.
    AlphaMismatch,
    /// A format with no alpha channel, where coverage was needed.
    AlphaRequired,
    /// A matrix that means "these samples are already RGB" was paired with a
    /// format that is not, or the other way round.
    MatrixMismatch,
    /// A pixel aspect ratio is zero or negative.
    BadPixelAspect,
    /// The wrong number of planes was offered for the format.
    PlaneCountMismatch,
    /// A plane's stride is narrower than its row.
    StrideTooNarrow,
    /// A plane holds a different number of bytes than its geometry and stride
    /// require.
    PlaneSizeMismatch,
    /// A frame is larger than the policy bound.
    FrameTooLarge,
    /// A pattern was asked for in a format it does not draw.
    PatternFormatUnsupported,
    /// A frame is larger than the whole pool.
    FrameLargerThanPool,
    /// A key was offered twice with different frames.
    KeyAlreadyPresent,
}

impl MediaStatus {
    /// One line naming the condition.
    #[must_use]
    pub const fn describe(self) -> &'static str {
        match self {
            Self::Time(status) => status.describe(),
            Self::OutOfMemory => "the allocation could not be satisfied",
            Self::EmptyGeometry => "a frame cannot have a zero dimension",
            Self::GeometryTooLarge => "the geometry is past the policy bound",
            Self::OddDimension => "this format halves a dimension that is odd",
            Self::SitingMismatch => "the chroma siting does not match the format",
            Self::AlphaMismatch => "the alpha association does not match the format",
            Self::AlphaRequired => "that format has no coverage to set",
            Self::MatrixMismatch => "the matrix does not match the format",
            Self::BadPixelAspect => "a pixel aspect ratio must be greater than zero",
            Self::PlaneCountMismatch => "the wrong number of planes for this format",
            Self::StrideTooNarrow => "a plane's stride is narrower than its row",
            Self::PlaneSizeMismatch => "a plane's size does not match its geometry",
            Self::FrameTooLarge => "the frame is past the policy bound",
            Self::PatternFormatUnsupported => "this pattern does not draw that format",
            Self::FrameLargerThanPool => "the frame is larger than the whole pool",
            Self::KeyAlreadyPresent => "that key already names a different frame",
        }
    }
}

impl From<CoreStatus> for MediaStatus {
    fn from(status: CoreStatus) -> Self {
        Self::Time(status)
    }
}

impl core::fmt::Display for MediaStatus {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        formatter.write_str(self.describe())
    }
}

/// The result of any fallible operation in this crate.
pub type Result<T> = core::result::Result<T, MediaStatus>;
