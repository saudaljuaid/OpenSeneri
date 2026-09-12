// SPDX-License-Identifier: GPL-3.0-only
//! What the numbers in a frame mean.
//!
//! R-8.2 says a frame carries its full description and an untagged frame is
//! refused. This module goes one step further and makes an untagged frame
//! *unrepresentable*: there is no `Unknown`, no `Unspecified`, and no
//! `Default` anywhere in it. A caller that does not know a clip's primaries
//! cannot express that by leaving them out; it has to decide, and the decision
//! is recorded where the next person can see it.
//!
//! That is not pedantry. Every washed-out export and every crushed black in
//! this industry is a frame whose description someone guessed.
//!
//! None of these enumerations is `#[non_exhaustive]`, and none of Media Editor's
//! are: these crates are one program released together, not a library with
//! downstream users, so adding a primary should be a compile error everywhere
//! that maps one rather than a silent `_` arm. That is R-1.2 enforced by the
//! compiler — widening a contract is a new contract, and here the build says
//! so.

/// The chromaticities the red, green, and blue in a frame refer to.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Primaries {
    /// ITU-R BT.709, which is what high definition means.
    Bt709,
    /// ITU-R BT.601 as broadcast in 525-line territories.
    Bt601Ntsc,
    /// ITU-R BT.601 as broadcast in 625-line territories.
    Bt601Pal,
    /// ITU-R BT.2020, ultra high definition.
    Bt2020,
    /// DCI-P3, the digital cinema projector gamut.
    DciP3,
    /// Display P3, which is DCI-P3 with a different white point.
    DisplayP3,
    /// ACES AP0, the encompassing academy gamut.
    AcesAp0,
    /// ACES AP1, the working academy gamut.
    AcesAp1,
}

/// The relationship between stored code values and light.
///
/// Often called gamma. It is not a synonym: a gamma is one shape this can
/// take, and the two most important modern ones are not gammas at all.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum TransferFunction {
    /// ITU-R BT.709's camera transfer.
    Bt709,
    /// The sRGB transfer, which is close to BT.709 and not the same.
    Srgb,
    /// ITU-R BT.2020 at ten bits.
    Bt2020Ten,
    /// SMPTE ST 2084, perceptual quantiser: high dynamic range, absolute.
    PerceptualQuantiser,
    /// ITU-R BT.2100 hybrid log-gamma: high dynamic range, relative.
    HybridLogGamma,
    /// Light in, light out.
    Linear,
    /// A pure power function of 2.2.
    Gamma22,
    /// A pure power function of 2.6, as digital cinema uses.
    Gamma26,
}

/// How luma and chroma relate to red, green, and blue.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum MatrixCoefficients {
    /// The samples are already red, green, and blue.
    Identity,
    /// ITU-R BT.709.
    Bt709,
    /// ITU-R BT.601.
    Bt601,
    /// ITU-R BT.2020, non-constant luminance.
    Bt2020NonConstant,
}

/// Whether code values use the whole interval or the broadcast one.
///
/// Getting this wrong is the classic washed-out or crushed picture, and it is
/// wrong most often because it was not written down.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Range {
    /// 16 to 235 for luma, 16 to 240 for chroma, at eight bits.
    Limited,
    /// 0 to 255 at eight bits.
    Full,
}

/// Where a chroma sample sits relative to the luma samples it covers.
///
/// Only meaningful for a subsampled format, which is why a frame carries it as
/// an option and refuses the mismatch either way.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum ChromaSiting {
    /// Horizontally co-sited with the left luma sample, vertically centred.
    Left,
    /// Centred between luma samples in both directions.
    Centre,
    /// Co-sited with the top-left luma sample.
    TopLeft,
}

/// Whether a frame's colour samples have been multiplied by its alpha.
///
/// The `over` operator is only correct — and only associative — on
/// premultiplied values. Compositing straight-alpha samples directly is the
/// classic dark fringe around a title, and it happens because somebody did not
/// know which kind they had.
///
/// So a frame with an alpha channel says which kind it is, in the same place
/// it says what its primaries are, and for the same reason.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum AlphaState {
    /// Colour samples stand on their own; alpha says how much of them to use.
    Straight,
    /// Colour samples have already been multiplied by alpha.
    Premultiplied,
}

/// Everything about a frame's colour, with nothing left out.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ColourDescription {
    /// The chromaticities.
    pub primaries: Primaries,
    /// The transfer function.
    pub transfer: TransferFunction,
    /// The luma-chroma matrix.
    pub matrix: MatrixCoefficients,
    /// The code value range.
    pub range: Range,
}

impl ColourDescription {
    /// Build a description.
    #[must_use]
    pub const fn new(
        primaries: Primaries,
        transfer: TransferFunction,
        matrix: MatrixCoefficients,
        range: Range,
    ) -> Self {
        Self {
            primaries,
            transfer,
            matrix,
            range,
        }
    }

    /// High definition video as it is normally delivered.
    #[must_use]
    pub const fn bt709_limited() -> Self {
        Self::new(
            Primaries::Bt709,
            TransferFunction::Bt709,
            MatrixCoefficients::Bt709,
            Range::Limited,
        )
    }

    /// Full-range sRGB, which is what a graphic or a title arrives as.
    #[must_use]
    pub const fn srgb_full() -> Self {
        Self::new(
            Primaries::Bt709,
            TransferFunction::Srgb,
            MatrixCoefficients::Identity,
            Range::Full,
        )
    }
}
