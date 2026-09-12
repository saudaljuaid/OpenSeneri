// SPDX-License-Identifier: GPL-3.0-only
//! A decoded picture, and everything true about it.
//!
//! A frame is immutable once produced (R-8.1). There is no method here that
//! changes one: an effect makes a new frame, which is what lets a frame be
//! cached by its digest and shared without a lock.
//!
//! The digest is computed at construction, over the description and every
//! sample, so two frames are the same frame exactly when they are the same
//! picture described the same way. That is what makes it usable as a cache key
//! (R-8.5) rather than merely as a checksum.

use alloc::borrow::Cow;
use alloc::vec;
use alloc::vec::Vec;

use media_editor_core::{Digest, Rational, Sha256};

use crate::colour::{AlphaState, ChromaSiting, ColourDescription, MatrixCoefficients, Range};
use crate::format::PixelFormat;
use crate::geometry::Geometry;
use crate::status::{MediaStatus, Result};

/// The most bytes one frame may occupy.
///
/// Sixty-four mebibytes: a 4K frame at eight bits and 4:4:4 is about
/// twenty-five, so this is room for one and a margin, and far more than a
/// Phipia program can map today. It is here so a hostile file cannot ask for a
/// frame the size of memory before anything checks (R-11.2).
pub const MAX_FRAME_BYTES: usize = 64 * 1024 * 1024;

/// Everything about a frame that is not its samples.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct FrameDescription {
    geometry: Geometry,
    format: PixelFormat,
    colour: ColourDescription,
    siting: Option<ChromaSiting>,
    alpha: Option<AlphaState>,
    pixel_aspect: Rational,
}

impl FrameDescription {
    /// Describe a frame.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::OddDimension`] if the format cannot express the
    /// geometry, [`MediaStatus::SitingMismatch`] if a subsampled format has no
    /// siting or an unsubsampled one has a siting,
    /// [`MediaStatus::MatrixMismatch`] if an identity matrix is paired with a
    /// luma-chroma format or the other way round, and
    /// [`MediaStatus::BadPixelAspect`] for a pixel aspect that is not
    /// positive.
    pub fn new(
        geometry: Geometry,
        format: PixelFormat,
        colour: ColourDescription,
        siting: Option<ChromaSiting>,
        alpha: Option<AlphaState>,
        pixel_aspect: Rational,
    ) -> Result<Self> {
        format.accepts(geometry)?;
        if format.is_subsampled() != siting.is_some() {
            return Err(MediaStatus::SitingMismatch);
        }
        if format.has_alpha() != alpha.is_some() {
            return Err(MediaStatus::AlphaMismatch);
        }
        if format.is_rgb() != (colour.matrix == MatrixCoefficients::Identity) {
            return Err(MediaStatus::MatrixMismatch);
        }
        if !pixel_aspect.is_positive() {
            return Err(MediaStatus::BadPixelAspect);
        }
        if format.packed_frame_bytes(geometry)? > MAX_FRAME_BYTES {
            return Err(MediaStatus::FrameTooLarge);
        }
        Ok(Self {
            geometry,
            format,
            colour,
            siting,
            alpha,
            pixel_aspect,
        })
    }

    /// The same description with square pixels, which most things have.
    ///
    /// # Errors
    ///
    /// As [`FrameDescription::new`].
    pub fn square(
        geometry: Geometry,
        format: PixelFormat,
        colour: ColourDescription,
        siting: Option<ChromaSiting>,
        alpha: Option<AlphaState>,
    ) -> Result<Self> {
        Self::new(geometry, format, colour, siting, alpha, Rational::ONE)
    }

    /// How big.
    #[must_use]
    pub const fn geometry(&self) -> Geometry {
        self.geometry
    }

    /// How it is laid out.
    #[must_use]
    pub const fn format(&self) -> PixelFormat {
        self.format
    }

    /// What the numbers mean.
    #[must_use]
    pub const fn colour(&self) -> ColourDescription {
        self.colour
    }

    /// Where chroma sits, for a subsampled format.
    #[must_use]
    pub const fn siting(&self) -> Option<ChromaSiting> {
        self.siting
    }

    /// Whether the colour samples have been multiplied by alpha.
    ///
    /// `None` for a format with no alpha channel, which is not the same as
    /// straight: there is nothing to associate.
    #[must_use]
    pub const fn alpha(&self) -> Option<AlphaState> {
        self.alpha
    }

    /// The same description with a different alpha association.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::AlphaMismatch`] for a format with no alpha channel.
    pub fn with_alpha(&self, alpha: AlphaState) -> Result<Self> {
        Self::new(
            self.geometry,
            self.format,
            self.colour,
            self.siting,
            Some(alpha),
            self.pixel_aspect,
        )
    }

    /// The shape of one pixel.
    #[must_use]
    pub const fn pixel_aspect(&self) -> Rational {
        self.pixel_aspect
    }

    /// How many bytes a tightly packed frame of this description holds.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::GeometryTooLarge`].
    pub fn packed_bytes(&self) -> Result<usize> {
        self.format.packed_frame_bytes(self.geometry)
    }

    /// Absorb this description into a hash, so that two frames that differ
    /// only in what their numbers mean have different digests.
    fn absorb(&self, hasher: &mut Sha256) {
        hasher.update(&self.geometry.width().to_le_bytes());
        hasher.update(&self.geometry.height().to_le_bytes());
        hasher.update(&[
            format_tag(self.format),
            primaries_tag(self.colour),
            transfer_tag(self.colour),
            matrix_tag(self.colour),
            range_tag(self.colour),
            siting_tag(self.siting),
            alpha_tag(self.alpha),
        ]);
        hasher.update(&self.pixel_aspect.numerator().to_le_bytes());
        hasher.update(&self.pixel_aspect.denominator().to_le_bytes());
    }
}

/// One plane of samples, and how far apart its rows are.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Plane {
    samples: Vec<u8>,
    stride: usize,
}

impl Plane {
    /// A plane of samples.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::StrideTooNarrow`] if rows would overlap.
    pub fn new(samples: Vec<u8>, stride: usize) -> Result<Self> {
        if stride == 0 {
            return Err(MediaStatus::StrideTooNarrow);
        }
        Ok(Self { samples, stride })
    }

    /// The samples.
    #[must_use]
    pub fn samples(&self) -> &[u8] {
        &self.samples
    }

    /// How many bytes from the start of one row to the start of the next.
    #[must_use]
    pub const fn stride(&self) -> usize {
        self.stride
    }

    /// The samples, handed over rather than lent.
    ///
    /// The other half of [`Plane::new`], and the reason both exist: a plane
    /// built out of a buffer and taken apart again gives the *same* buffer
    /// back, so a program that fills a window, wraps it, uses it and unwraps
    /// it has copied nothing and allocated once.
    #[must_use]
    pub fn into_samples(self) -> Vec<u8> {
        self.samples
    }

    /// One row.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::PlaneSizeMismatch`] if the row is not there.
    pub fn row(&self, index: usize) -> Result<&[u8]> {
        let start = index
            .checked_mul(self.stride)
            .ok_or(MediaStatus::PlaneSizeMismatch)?;
        let end = start
            .checked_add(self.stride)
            .ok_or(MediaStatus::PlaneSizeMismatch)?;
        self.samples
            .get(start..end)
            .ok_or(MediaStatus::PlaneSizeMismatch)
    }
}

/// A decoded picture.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Frame {
    description: FrameDescription,
    planes: Vec<Plane>,
    digest: Digest,
    bytes: usize,
}

impl Frame {
    /// Build a frame from planes.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::PlaneCountMismatch`],
    /// [`MediaStatus::StrideTooNarrow`], or
    /// [`MediaStatus::PlaneSizeMismatch`] if any plane does not describe the
    /// geometry it claims to.
    pub fn new(description: FrameDescription, planes: Vec<Plane>) -> Result<Self> {
        let format = description.format();
        if planes.len() != format.plane_count() {
            return Err(MediaStatus::PlaneCountMismatch);
        }
        let mut bytes = 0_usize;
        for (index, plane) in planes.iter().enumerate() {
            let plane_geometry = format.plane_geometry(description.geometry(), index)?;
            let row = format.plane_row_bytes(description.geometry(), index)?;
            if plane.stride() < row {
                return Err(MediaStatus::StrideTooNarrow);
            }
            let rows = usize::try_from(plane_geometry.height())
                .map_err(|_| MediaStatus::GeometryTooLarge)?;
            let required = plane
                .stride()
                .checked_mul(rows)
                .ok_or(MediaStatus::GeometryTooLarge)?;
            if plane.samples().len() != required {
                return Err(MediaStatus::PlaneSizeMismatch);
            }
            bytes = bytes
                .checked_add(required)
                .ok_or(MediaStatus::FrameTooLarge)?;
        }
        if bytes > MAX_FRAME_BYTES {
            return Err(MediaStatus::FrameTooLarge);
        }

        // The digest covers the description and every sample a row actually
        // holds - not the padding between rows, which is not part of the
        // picture and which two encoders may fill differently.
        let mut hasher = Sha256::new();
        description.absorb(&mut hasher);
        for (index, plane) in planes.iter().enumerate() {
            let plane_geometry = format.plane_geometry(description.geometry(), index)?;
            let row = format.plane_row_bytes(description.geometry(), index)?;
            for line in 0..plane_geometry.height() {
                let line = usize::try_from(line).map_err(|_| MediaStatus::GeometryTooLarge)?;
                let whole = plane.row(line)?;
                hasher.update(whole.get(..row).ok_or(MediaStatus::PlaneSizeMismatch)?);
            }
        }

        Ok(Self {
            description,
            planes,
            digest: hasher.finish(),
            bytes,
        })
    }

    /// Build a frame from tightly packed bytes, in plane order.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::PlaneSizeMismatch`] if the slice is not exactly the
    /// packed size, or [`MediaStatus::OutOfMemory`].
    pub fn from_packed(description: FrameDescription, bytes: &[u8]) -> Result<Self> {
        if bytes.len() != description.packed_bytes()? {
            return Err(MediaStatus::PlaneSizeMismatch);
        }
        let format = description.format();
        let mut planes = Vec::new();
        planes
            .try_reserve(format.plane_count())
            .map_err(|_| MediaStatus::OutOfMemory)?;
        let mut offset = 0_usize;
        for index in 0..format.plane_count() {
            let plane_geometry = format.plane_geometry(description.geometry(), index)?;
            let row = format.plane_row_bytes(description.geometry(), index)?;
            let rows = usize::try_from(plane_geometry.height())
                .map_err(|_| MediaStatus::GeometryTooLarge)?;
            let length = row.checked_mul(rows).ok_or(MediaStatus::GeometryTooLarge)?;
            let slice = bytes
                .get(offset..offset + length)
                .ok_or(MediaStatus::PlaneSizeMismatch)?;
            let mut samples = Vec::new();
            samples
                .try_reserve(length)
                .map_err(|_| MediaStatus::OutOfMemory)?;
            samples.extend_from_slice(slice);
            planes.push(Plane::new(samples, row)?);
            offset += length;
        }
        Self::new(description, planes)
    }

    /// An all-zero frame of a description.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::OutOfMemory`], or a description refusal.
    pub fn blank(description: FrameDescription) -> Result<Self> {
        let bytes = description.packed_bytes()?;
        let mut samples = Vec::new();
        samples
            .try_reserve(bytes)
            .map_err(|_| MediaStatus::OutOfMemory)?;
        samples.resize(bytes, 0);

        // Zero is not black, and the difference is the whole point of writing
        // this out. In a limited-range luma plane zero is *below* the legal
        // floor of sixteen — an illegal sample. In a chroma plane zero is not
        // neutral but minus one half on both axes, which is a saturated
        // blue-green: a "blank" frame filled with zeroes shows up on a
        // vectorscope in the corner of the graticule rather than at the
        // origin, and a scope test found exactly that.
        //
        // So a blank frame is what a blank frame means in an editing suite: an
        // opaque black slug, legal in its own range.
        let range = description.colour().range;
        let format = description.format();
        if format.is_rgb() {
            let black = black_code(range);
            let stride = usize::try_from(format.packed_bytes_per_pixel().unwrap_or(1))
                .map_err(|_| MediaStatus::GeometryTooLarge)?;
            for pixel in samples.chunks_exact_mut(stride) {
                for sample in pixel.iter_mut().take(COLOUR_CHANNELS.min(stride)) {
                    *sample = black;
                }
                if format.has_alpha() {
                    if let Some(alpha) = pixel.get_mut(ALPHA_CHANNEL) {
                        *alpha = FULL_COVERAGE;
                    }
                }
            }
        } else {
            let luma = format.plane_row_bytes(description.geometry(), 0)?;
            let rows = usize::try_from(description.geometry().height())
                .map_err(|_| MediaStatus::GeometryTooLarge)?;
            let luma_bytes = luma
                .checked_mul(rows)
                .ok_or(MediaStatus::GeometryTooLarge)?;
            for sample in samples.iter_mut().take(luma_bytes) {
                *sample = black_code(range);
            }
            for sample in samples.iter_mut().skip(luma_bytes) {
                *sample = NEUTRAL_CHROMA;
            }
        }
        Self::from_packed(description, &samples)
    }

    /// A frame of nothing at all: no light, and no coverage either.
    ///
    /// [`Frame::blank`] is an opaque black slug, which is what a *programme*
    /// shows where it has nothing — a viewer displays black leader and an
    /// export writes black frames. This is the other kind of nothing, and the
    /// two are not interchangeable: material that is absent is **absent**, not
    /// black, and a nested sequence composited onto black would blank out
    /// every track beneath it wherever it happened to be empty.
    ///
    /// The colour is black rather than zero, for the reason `blank` writes out
    /// at length: zero is below the legal floor of a limited-range plane. In
    /// premultiplied form that is also the *correct* value — a premultiplied
    /// sample is the encoding of `light × coverage`, and at no coverage the
    /// light is nought, whose encoding is the black code and not the byte
    /// nought. So this frame passes [`crate::AlphaState::Premultiplied`]'s own
    /// check rather than merely claiming to.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::AlphaRequired`] for a format with no alpha channel:
    /// there is nowhere to say "none of this is here", and a format without
    /// coverage cannot express absence at all.
    /// [`MediaStatus::OutOfMemory`], or a description refusal.
    pub fn clear(description: FrameDescription) -> Result<Self> {
        if !description.format().has_alpha() {
            return Err(MediaStatus::AlphaRequired);
        }
        let held = Self::blank(description)?;
        let mut samples = held.to_packed()?;
        let stride = usize::try_from(description.format().packed_bytes_per_pixel().unwrap_or(1))
            .map_err(|_| MediaStatus::GeometryTooLarge)?;
        for pixel in samples.chunks_exact_mut(stride) {
            if let Some(alpha) = pixel.get_mut(ALPHA_CHANNEL) {
                *alpha = 0;
            }
        }
        Self::from_packed(description, &samples)
    }

    /// What this frame is.
    #[must_use]
    pub const fn description(&self) -> &FrameDescription {
        &self.description
    }

    /// The planes.
    #[must_use]
    pub fn planes(&self) -> &[Plane] {
        &self.planes
    }

    /// One plane.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::PlaneCountMismatch`].
    pub fn plane(&self, index: usize) -> Result<&Plane> {
        self.planes
            .get(index)
            .ok_or(MediaStatus::PlaneCountMismatch)
    }

    /// The digest of this picture and its description.
    #[must_use]
    pub const fn digest(&self) -> Digest {
        self.digest
    }

    /// How much memory this frame occupies.
    #[must_use]
    pub const fn bytes(&self) -> usize {
        self.bytes
    }

    /// Whether the frame already holds its samples as one packed run.
    ///
    /// True for an interleaved format with no padding between rows, which is
    /// every frame the row path carries; false for a planar format, whose
    /// planes are separate buffers and can never be one slice, and for a
    /// frame with a stride wider than its rows.
    ///
    /// Public because it is the difference between [`Frame::packed`] lending
    /// and copying, and a caller on a small machine is entitled to know which
    /// it is about to get rather than to hope.
    #[must_use]
    pub fn is_packed(&self) -> bool {
        self.run().is_some()
    }

    /// The samples as one packed run, if the frame already holds them so.
    fn run(&self) -> Option<&[u8]> {
        let [plane] = self.planes.as_slice() else {
            return None;
        };
        let row = self
            .description
            .format()
            .plane_row_bytes(self.description.geometry(), 0)
            .ok()?;
        (plane.stride() == row).then(|| plane.samples())
    }

    /// The frame's samples, packed, **lent** when they are already packed.
    ///
    /// The whole of what a row-at-a-time program needs from this type. A row
    /// of an interleaved picture is one run of bytes and the frame is already
    /// holding it: copying it out to look at it doubles the largest thing in
    /// memory to say nothing new.
    ///
    /// It is [`Cow`] rather than a refusal because the two answers are the
    /// same bytes. A planar frame cannot be one slice — its planes are
    /// separate buffers — so somebody has to pack it, and making every caller
    /// write that branch would be making every caller reimplement this
    /// function. What a caller *can* do, if it cannot afford the copy, is ask
    /// [`Frame::is_packed`] first.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::OutOfMemory`], and only on the copying side.
    pub fn packed(&self) -> Result<Cow<'_, [u8]>> {
        match self.run() {
            Some(run) => Ok(Cow::Borrowed(run)),
            None => Ok(Cow::Owned(self.to_packed()?)),
        }
    }

    /// A frame that **takes** a buffer rather than reading one.
    ///
    /// The other end of [`Frame::into_packed`], and the pair is the point: a
    /// window filled from storage becomes a frame, becomes a window again, and
    /// no sample is copied at either boundary. That is what lets a row path
    /// allocate once and then cycle one buffer for a whole reel, on a machine
    /// that is given seventy-six kibibytes.
    ///
    /// A planar frame copies, because a planar frame's planes are separate
    /// buffers and one `Vec` cannot become three without moving bytes. That
    /// is not a refusal for the reason [`Frame::packed`] gives: the answer is
    /// the same frame either way, and a caller that cannot afford the copy
    /// knows from the format alone.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::PlaneSizeMismatch`] if the buffer is not exactly the
    /// packed size, or [`MediaStatus::OutOfMemory`].
    pub fn from_owned(description: FrameDescription, samples: Vec<u8>) -> Result<Self> {
        // No length check of its own. A buffer of the wrong size is refused
        // either by `from_packed` on the planar path or by `Frame::new` on the
        // one-plane path, both with `PlaneSizeMismatch` — the same status this
        // would have raised, one step later, having done no work in between.
        // A control found the check changed no answer, and this project has
        // deleted eight of those.
        let format = description.format();
        if format.plane_count() != 1 {
            return Self::from_packed(description, &samples);
        }
        let row = format.plane_row_bytes(description.geometry(), 0)?;
        let mut planes = Vec::new();
        planes
            .try_reserve(1)
            .map_err(|_| MediaStatus::OutOfMemory)?;
        planes.push(Plane::new(samples, row)?);
        Self::new(description, planes)
    }

    /// The frame's samples, packed, **handed over** rather than copied.
    ///
    /// The frame is consumed, which is what makes this free: there is nobody
    /// left to lend to. A frame that is already one packed run gives its own
    /// buffer back — the same allocation, at the same address, as
    /// [`Frame::from_owned`] took.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::OutOfMemory`], and only on the copying side.
    pub fn into_packed(mut self) -> Result<Vec<u8>> {
        if self.run().is_none() {
            return self.to_packed();
        }
        let plane = self.planes.pop().ok_or(MediaStatus::PlaneCountMismatch)?;
        Ok(plane.into_samples())
    }

    /// The frame's samples, tightly packed, in plane order.
    ///
    /// Always a fresh buffer. [`Frame::packed`] is the one to reach for when
    /// the bytes are only going to be read, and [`Frame::into_packed`] when
    /// the frame is finished with.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::OutOfMemory`].
    pub fn to_packed(&self) -> Result<Vec<u8>> {
        let total = self.description.packed_bytes()?;
        let mut packed = vec![];
        packed
            .try_reserve(total)
            .map_err(|_| MediaStatus::OutOfMemory)?;
        let format = self.description.format();
        for (index, plane) in self.planes.iter().enumerate() {
            let plane_geometry = format.plane_geometry(self.description.geometry(), index)?;
            let row = format.plane_row_bytes(self.description.geometry(), index)?;
            for line in 0..plane_geometry.height() {
                let line = usize::try_from(line).map_err(|_| MediaStatus::GeometryTooLarge)?;
                let whole = plane.row(line)?;
                packed.extend_from_slice(whole.get(..row).ok_or(MediaStatus::PlaneSizeMismatch)?);
            }
        }
        Ok(packed)
    }
}

/// Stable tags, used only for the digest.
///
/// These are deliberately separate from any file format's tags: a frame's
/// digest must not change because a container renumbered something.
const fn format_tag(format: PixelFormat) -> u8 {
    match format {
        PixelFormat::Rgba8 => 1,
        PixelFormat::Rgb8 => 2,
        PixelFormat::Gray8 => 3,
        PixelFormat::Yuv420p8 => 4,
        PixelFormat::Yuv422p8 => 5,
        PixelFormat::Yuv444p8 => 6,
    }
}

const fn primaries_tag(colour: ColourDescription) -> u8 {
    use crate::colour::Primaries;
    match colour.primaries {
        Primaries::Bt709 => 1,
        Primaries::Bt601Ntsc => 2,
        Primaries::Bt601Pal => 3,
        Primaries::Bt2020 => 4,
        Primaries::DciP3 => 5,
        Primaries::DisplayP3 => 6,
        Primaries::AcesAp0 => 7,
        Primaries::AcesAp1 => 8,
    }
}

const fn transfer_tag(colour: ColourDescription) -> u8 {
    use crate::colour::TransferFunction;
    match colour.transfer {
        TransferFunction::Bt709 => 1,
        TransferFunction::Srgb => 2,
        TransferFunction::Bt2020Ten => 3,
        TransferFunction::PerceptualQuantiser => 4,
        TransferFunction::HybridLogGamma => 5,
        TransferFunction::Linear => 6,
        TransferFunction::Gamma22 => 7,
        TransferFunction::Gamma26 => 8,
    }
}

const fn matrix_tag(colour: ColourDescription) -> u8 {
    match colour.matrix {
        MatrixCoefficients::Identity => 1,
        MatrixCoefficients::Bt709 => 2,
        MatrixCoefficients::Bt601 => 3,
        MatrixCoefficients::Bt2020NonConstant => 4,
    }
}

/// How many of a packed pixel's bytes carry colour rather than coverage.
const COLOUR_CHANNELS: usize = 3;

/// Where the coverage byte sits in a packed pixel that has one.
const ALPHA_CHANNEL: usize = 3;

/// The byte that means fully covered.
const FULL_COVERAGE: u8 = 255;

/// The chroma code value that means no colour at all, in either range.
///
/// Chroma is stored offset so that it can be negative, and the offset is 128
/// whether the range is limited or full — so neutral is 128 either way, and
/// zero is the most negative value the byte can hold.
const NEUTRAL_CHROMA: u8 = 128;

/// The code value that means black, which is not zero in a limited range.
const fn black_code(range: Range) -> u8 {
    match range {
        Range::Full => 0,
        Range::Limited => 16,
    }
}

const fn range_tag(colour: ColourDescription) -> u8 {
    use crate::colour::Range;
    match colour.range {
        Range::Limited => 1,
        Range::Full => 2,
    }
}

const fn alpha_tag(alpha: Option<AlphaState>) -> u8 {
    match alpha {
        None => 0,
        Some(AlphaState::Straight) => 1,
        Some(AlphaState::Premultiplied) => 2,
    }
}

const fn siting_tag(siting: Option<ChromaSiting>) -> u8 {
    match siting {
        None => 0,
        Some(ChromaSiting::Left) => 1,
        Some(ChromaSiting::Centre) => 2,
        Some(ChromaSiting::TopLeft) => 3,
    }
}
