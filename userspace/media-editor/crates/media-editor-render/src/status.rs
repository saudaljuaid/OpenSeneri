// SPDX-License-Identifier: GPL-3.0-only
//! Every way the colour pipeline refuses.

use media_editor_core::CoreStatus;
use media_editor_media::MediaStatus;

/// A refusal from the render side.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum RenderStatus {
    /// An exact arithmetic refusal from the core types.
    Time(CoreStatus),
    /// The media types refused.
    Media(MediaStatus),
    /// An allocation the caller must handle.
    OutOfMemory,
    /// A matrix has no inverse.
    Singular,
    /// A chromaticity's y coordinate is zero, so it names no colour.
    DegenerateChromaticity,
    /// A set of primaries and a white point that do not span a gamut.
    DegenerateGamut,
    /// A conversion between two descriptions that this build cannot do
    /// exactly.
    ConversionUnavailable,
    /// A value outside the domain a transfer function defines.
    OutsideDomain,
    /// A node identifier names nothing in the graph.
    UnknownNode,
    /// A graph is at its policy capacity.
    GraphTooLarge,
    /// Compositing was asked of a format with no alpha channel.
    AlphaRequired,
    /// A frame is straight where premultiplied was required, or the reverse.
    WrongAlphaState,
    /// A frame calls itself premultiplied and holds colour brighter than its
    /// own coverage, which premultiplied samples cannot.
    NotPremultiplied,
    /// Two frames that do not describe the same picture cannot be layers of
    /// one.
    NotComposable,
    /// A lookup table with a side outside the range this build carries.
    LutSizeUnsupported,
    /// A lookup table whose sample count is not the cube of its size.
    LutNotACube,
    /// A lattice point outside a lookup table.
    LutIndexOutOfRange,
    /// A frame in a different encoding from the one a look was authored for.
    LookSpaceMismatch,
    /// A look asked of a format that is not red-green-blue.
    LookNotRgb,
    /// A look asked of premultiplied coverage, where a non-linear function
    /// computes the wrong thing at every coverage but full.
    LookPremultiplied,
    /// A grade asked for at a strength outside none of it to all of it.
    LookStrengthOutOfRange,
    /// A source answered with a frame that is not the one asked for.
    SourceDescriptionMismatch,
    /// A library was asked for media it does not hold.
    MediaAbsent,
    /// A library holds that media and cannot read it as material.
    ///
    /// Separate from [`RenderStatus::MediaAbsent`] deliberately: one of them
    /// means somebody's drive is not mounted and the other means somebody's
    /// file is damaged, and telling a person the first when it is the second
    /// sends them looking in the wrong place.
    MediaUnreadable,
    /// Media that holds no frame at the tick asked for.
    FrameAbsent,
    /// A library was asked for a look it does not hold.
    LookAbsent,
    /// A node whose output row is not independent of its neighbours, asked for
    /// a row.
    ///
    /// A statement about the *format*, and now only about the format: one
    /// chroma row of a subsampled frame serves two luma rows, so a luma row
    /// cannot be produced without its neighbour and no slicing changes that.
    ///
    /// It used to cover resampling too, and that was overclaiming. A turn's
    /// row does read more than a band — across the whole width. Across a
    /// narrow enough strip it does not, and a row drawn in strips is the same
    /// row. What can still refuse there is [`RenderStatus::BandTooTall`],
    /// which is about one row of one map rather than about the operation.
    ///
    /// Distinct from [`RenderStatus::NoRowForm`], which is a statement about
    /// the build.
    NotRowLocal,
    /// A band of source rows taller than one may be.
    ///
    /// A vertical downscale steeper than [`crate::resample::MAX_BAND_ROWS`] to
    /// one, where a band is most of a frame and scanning has bought nothing.
    /// Narrowing the strip does not help, because a map that takes horizontals
    /// to horizontals reads the same band however much of the row is asked
    /// for — which is exactly the case this refuses.
    BandTooTall,
    /// A resampled row reached for a source row outside the band it was given.
    ///
    /// Not a bad argument from a caller but a wrong answer from
    /// [`crate::resample::band`], and the only way to notice it: a band that
    /// came back too short would otherwise draw transparency where there is
    /// picture, and transparency is what a source legitimately returns past
    /// its own edge.
    RowOutsideBand,
    /// A node this build has no row form for, asked for a row.
    ///
    /// A statement about what has been written, not about what is possible.
    /// The two are separate because somebody reading a refusal needs to know
    /// whether to wait for a later version or to change what they are asking
    /// for.
    NoRowForm,
    /// An edge whose coefficients name no line.
    DegenerateEdge,
    /// A shape with no edges, or one enclosing no area.
    DegenerateShape,
    /// A shape with more edges than the rasteriser carries.
    ShapeTooComplex,
    /// A coverage plane whose size does not match the frame it is for.
    CoverageSizeMismatch,
    /// A character this face has no glyph for.
    NoSuchGlyph,
    /// A run of text longer than this draws.
    TextTooLong,
    /// A type size at or below nothing.
    SizeNotPositive,
}

impl RenderStatus {
    /// One line naming the condition.
    #[must_use]
    pub const fn describe(self) -> &'static str {
        match self {
            Self::Time(status) => status.describe(),
            Self::Media(status) => status.describe(),
            Self::OutOfMemory => "the allocation could not be satisfied",
            Self::Singular => "the matrix has no inverse",
            Self::DegenerateChromaticity => "a chromaticity with a zero y names no colour",
            Self::DegenerateGamut => "those primaries and that white point span no gamut",
            Self::ConversionUnavailable => "this build cannot do that conversion exactly",
            Self::OutsideDomain => "that value is outside the transfer function's domain",
            Self::UnknownNode => "the identifier names no node in this graph",
            Self::GraphTooLarge => "the graph is at its capacity",
            Self::AlphaRequired => "compositing needs an alpha channel and this format has none",
            Self::WrongAlphaState => "the frame's alpha association is not the one required",
            Self::NotPremultiplied => "this frame's colour is brighter than its own coverage",
            Self::NotComposable => "these two frames do not describe the same picture",
            Self::LutSizeUnsupported => "that lookup table side is not one this build carries",
            Self::LutNotACube => "that many samples is not the cube of that size",
            Self::LutIndexOutOfRange => "that lattice point is outside the table",
            Self::LookSpaceMismatch => "that frame is not in the encoding this look was made for",
            Self::LookNotRgb => "a look maps three colour channels, and that format has other ones",
            Self::LookPremultiplied => "a look needs straight coverage, not premultiplied",
            Self::LookStrengthOutOfRange => {
                "a grade's strength runs from none of the look to all of it"
            }
            Self::SourceDescriptionMismatch => "the source answered with a different frame",
            Self::MediaAbsent => "this library does not hold that media",
            Self::MediaUnreadable => "that media is here and cannot be read as material",
            Self::FrameAbsent => "that media holds no frame at that tick",
            Self::LookAbsent => "this library does not hold that look",
            Self::NotRowLocal => "an output row of that node is not independent of its neighbours",
            Self::BandTooTall => "one output row reads more source rows than a band holds",
            Self::RowOutsideBand => "a resampled row reached past the band it was given",
            Self::NoRowForm => "this build cannot produce that a row at a time",
            Self::DegenerateEdge => "an edge with no direction names no line",
            Self::DegenerateShape => "that shape encloses nothing",
            Self::ShapeTooComplex => "that shape has more edges than this rasterises",
            Self::CoverageSizeMismatch => "the coverage plane is not the size of the frame",
            Self::NoSuchGlyph => "this face has no glyph for that character",
            Self::TextTooLong => "that run of text is longer than this draws",
            Self::SizeNotPositive => "type has to be some size",
        }
    }
}

impl From<CoreStatus> for RenderStatus {
    fn from(status: CoreStatus) -> Self {
        Self::Time(status)
    }
}

impl From<MediaStatus> for RenderStatus {
    fn from(status: MediaStatus) -> Self {
        Self::Media(status)
    }
}

impl core::fmt::Display for RenderStatus {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        formatter.write_str(self.describe())
    }
}

/// The result of any fallible operation in this crate.
pub type Result<T> = core::result::Result<T, RenderStatus>;
