// SPDX-License-Identifier: GPL-3.0-only
//! The one picture format Phipia and Media Editor already agree on.
//!
//! Phipia's Media Editor workspace imports "an ordinary uncompressed 24-bit BMP
//! from the current Files directory on the writable data volume", bounded at
//! 1920×1080, with every header field and row offset validated. This is the
//! same decoder, written in safe Rust against the same bounds — so a file
//! Phipia's importer accepts is a file this accepts, and a file it refuses is
//! refused here for the same reason and under a name that says which.
//!
//! Read from `src/kernel/ui.c`, `media_source_load_preview`, and from
//! `docs/FIRST_ENVIRONMENT.md`.
//!
//! ## Why BMP, of all things
//!
//! Because it is the only raster format with no entropy coding, no colour
//! management, no chunk structure and no optional features — which makes it
//! the only one that can be decoded correctly without a library, and this
//! project may not have one (R-11.3). It is not a good format. It is a format
//! whose whole contents can be argued about in one page, which is the property
//! that matters at this end of the project.
//!
//! ## The layout, and which of it is checked
//!
//! ```text
//! offset  size  field                       checked
//! 0       2     "BM"                        yes
//! 2       4     file bytes                  no -- the row arithmetic decides
//! 6       4     reserved                    no
//! 10      4     offset to the pixels        yes, against the header and size
//! 14      4     information header bytes    yes, at least 40
//! 18      4     width, signed               yes, positive and bounded
//! 22      4     height, signed              yes, non-zero and bounded
//! 26      2     planes                      yes, exactly one
//! 28      2     bits per pixel              yes, exactly 24
//! 30      4     compression                 yes, exactly none
//! ```
//!
//! A **negative height** means the rows run top-down; the usual positive one
//! means they run bottom-up, which is the format's oldest wart and the one
//! thing about it that catches everybody once.

use alloc::vec::Vec;

use media_editor_media::colour::{
    ColourDescription, MatrixCoefficients, Primaries, Range, TransferFunction,
};
use media_editor_media::{Frame, FrameDescription, Geometry, PixelFormat, Plane};

use crate::bytes::Reader;
use crate::status::{IoStatus, Result};

/// How many bytes the two headers take together.
pub const HEADER_BYTES: usize = 54;

/// The widest picture this accepts. Phipia's `UI_MEDIA_SOURCE_BMP_MAX_WIDTH`.
pub const MAX_WIDTH: u32 = 1920;

/// The tallest picture this accepts. Phipia's `UI_MEDIA_SOURCE_BMP_MAX_HEIGHT`.
pub const MAX_HEIGHT: u32 = 1080;

/// Decode a 24-bit uncompressed BMP into a frame.
///
/// The frame is `Rgb8`, in sRGB, full range — which is what a BMP is, and
/// saying so once here is what stops every later stage from guessing.
///
/// # Errors
///
/// [`IoStatus::NotABitmap`] for anything that is not one,
/// [`IoStatus::BitmapUnsupported`] for a bitmap this build does not read —
/// a palette, a compression, a bit depth other than 24 —
/// [`IoStatus::BitmapTooLarge`] past the bounds above,
/// [`IoStatus::TruncatedField`] for a file that ends inside its pixels, and
/// [`IoStatus::Media`] for anything the frame itself refuses.
pub fn decode(file: &[u8]) -> Result<Frame> {
    let head = header(file)?;
    let stride = usize::try_from(head.width).map_err(|_| IoStatus::TooMany)? * 3;
    // Rows are padded to a multiple of four bytes. This is the field the
    // format is most often got wrong in, because at a width that happens to be
    // a multiple of four the padding is nought and a decoder that forgot it
    // works perfectly on the test image.
    let padded = stride
        .checked_add(3)
        .ok_or(IoStatus::TooMany)?
        .checked_div(4)
        .ok_or(IoStatus::TooMany)?
        .checked_mul(4)
        .ok_or(IoStatus::TooMany)?;
    let height = usize::try_from(head.height).map_err(|_| IoStatus::TooMany)?;
    let wanted = padded.checked_mul(height).ok_or(IoStatus::TooMany)?;
    let pixels = file
        .get(head.offset..)
        .ok_or(IoStatus::TruncatedField)?
        .get(..wanted)
        .ok_or(IoStatus::TruncatedField)?;

    let geometry = Geometry::new(head.width, head.height).map_err(IoStatus::Media)?;
    let description = FrameDescription::new(
        geometry,
        PixelFormat::Rgb8,
        ColourDescription {
            primaries: Primaries::Bt709,
            transfer: TransferFunction::Srgb,
            matrix: MatrixCoefficients::Identity,
            range: Range::Full,
        },
        None,
        None,
        media_editor_core::Rational::ONE,
    )
    .map_err(IoStatus::Media)?;

    let mut samples = Vec::new();
    samples
        .try_reserve(stride.checked_mul(height).ok_or(IoStatus::TooMany)?)
        .map_err(|_| IoStatus::OutOfMemory)?;
    for row in 0..height {
        // Bottom-up unless the height said otherwise, which is the wart.
        let source = if head.top_down { row } else { height - 1 - row };
        let at = source.checked_mul(padded).ok_or(IoStatus::TooMany)?;
        let line = pixels
            .get(at..at + stride)
            .ok_or(IoStatus::TruncatedField)?;
        // And the channels are stored blue, green, red, which is the second
        // thing about this format that catches everybody.
        for pixel in line.chunks_exact(3) {
            samples.push(pixel[2]);
            samples.push(pixel[1]);
            samples.push(pixel[0]);
        }
    }
    let plane = Plane::new(samples, stride).map_err(IoStatus::Media)?;
    let mut planes = Vec::new();
    planes.try_reserve(1).map_err(|_| IoStatus::OutOfMemory)?;
    planes.push(plane);
    Frame::new(description, planes).map_err(IoStatus::Media)
}

/// What the two headers said, once every field of them has been believed.
struct Head {
    width: u32,
    height: u32,
    offset: usize,
    top_down: bool,
}

/// Read and check the headers.
fn header(file: &[u8]) -> Result<Head> {
    if file.len() < HEADER_BYTES {
        return Err(IoStatus::NotABitmap);
    }
    let mut reader = Reader::new(file);
    if reader.take(2)? != b"BM" {
        return Err(IoStatus::NotABitmap);
    }
    let _file_bytes = reader.u32()?;
    let _reserved = reader.u32()?;
    let offset = usize::try_from(reader.u32()?).map_err(|_| IoStatus::TooMany)?;
    if reader.u32()? < 40 {
        // A header shorter than the BITMAPINFOHEADER is one of the two
        // pre-Windows-3 shapes, which have a different field order entirely.
        return Err(IoStatus::BitmapUnsupported);
    }
    let width = reader.i32()?;
    let height = reader.i32()?;
    if reader.u16()? != 1 {
        return Err(IoStatus::BitmapUnsupported);
    }
    if reader.u16()? != 24 {
        // Not a bit depth this reads. Eight would need the palette that
        // follows the header, and thirty-two would need a decision about what
        // the fourth channel means, which BMP does not say.
        return Err(IoStatus::BitmapUnsupported);
    }
    if reader.u32()? != 0 {
        // Any compression at all, including the run-length encodings and the
        // bitfield forms.
        return Err(IoStatus::BitmapUnsupported);
    }
    if width <= 0 || height == 0 || height == i32::MIN {
        // `i32::MIN` has no positive counterpart, so a height of it cannot be
        // turned the right way up. Phipia refuses it by name and so does this.
        return Err(IoStatus::NotABitmap);
    }
    let top_down = height < 0;
    let height = height.unsigned_abs();
    let width = width.unsigned_abs();
    if width > MAX_WIDTH || height > MAX_HEIGHT {
        return Err(IoStatus::BitmapTooLarge);
    }
    if offset < HEADER_BYTES || offset > file.len() {
        return Err(IoStatus::NotABitmap);
    }
    Ok(Head {
        width,
        height,
        offset,
        top_down,
    })
}

/// Encode a frame as a 24-bit uncompressed BMP, bottom-up.
///
/// Bottom-up rather than top-down, and that is a compatibility decision rather
/// than a preference: it is the orientation every reader has always handled,
/// including the one in Phipia's export path, and this file is written to be
/// read by things that are not this program.
///
/// # Errors
///
/// [`IoStatus::BitmapUnsupported`] for a frame that is not `Rgb8`,
/// [`IoStatus::BitmapTooLarge`] past the bounds above, and
/// [`IoStatus::OutOfMemory`].
pub fn encode(frame: &Frame) -> Result<Vec<u8>> {
    let description = frame.description();
    if description.format() != PixelFormat::Rgb8 {
        return Err(IoStatus::BitmapUnsupported);
    }
    let geometry = description.geometry();
    if geometry.width() > MAX_WIDTH || geometry.height() > MAX_HEIGHT {
        return Err(IoStatus::BitmapTooLarge);
    }
    let width = usize::try_from(geometry.width()).map_err(|_| IoStatus::TooMany)?;
    let height = usize::try_from(geometry.height()).map_err(|_| IoStatus::TooMany)?;
    let stride = width * 3;
    let padded = stride.div_ceil(4) * 4;
    let pixels = padded.checked_mul(height).ok_or(IoStatus::TooMany)?;
    let total = HEADER_BYTES.checked_add(pixels).ok_or(IoStatus::TooMany)?;

    let mut file = Vec::new();
    file.try_reserve(total).map_err(|_| IoStatus::OutOfMemory)?;
    file.extend_from_slice(b"BM");
    file.extend_from_slice(
        &u32::try_from(total)
            .map_err(|_| IoStatus::TooMany)?
            .to_le_bytes(),
    );
    file.extend_from_slice(&0_u32.to_le_bytes());
    file.extend_from_slice(
        &u32::try_from(HEADER_BYTES)
            .map_err(|_| IoStatus::TooMany)?
            .to_le_bytes(),
    );
    file.extend_from_slice(&40_u32.to_le_bytes());
    file.extend_from_slice(
        &i32::try_from(width)
            .map_err(|_| IoStatus::TooMany)?
            .to_le_bytes(),
    );
    file.extend_from_slice(
        &i32::try_from(height)
            .map_err(|_| IoStatus::TooMany)?
            .to_le_bytes(),
    );
    file.extend_from_slice(&1_u16.to_le_bytes());
    file.extend_from_slice(&24_u16.to_le_bytes());
    file.extend_from_slice(&0_u32.to_le_bytes());
    file.extend_from_slice(
        &u32::try_from(pixels)
            .map_err(|_| IoStatus::TooMany)?
            .to_le_bytes(),
    );
    // Neither resolution field, both colour-table fields, all four nought.
    for _ in 0..4 {
        file.extend_from_slice(&0_u32.to_le_bytes());
    }

    let plane = frame.plane(0).map_err(IoStatus::Media)?;
    let samples = plane.samples();
    for row in (0..height).rev() {
        let at = row.checked_mul(plane.stride()).ok_or(IoStatus::TooMany)?;
        let line = samples
            .get(at..at + stride)
            .ok_or(IoStatus::TruncatedField)?;
        for pixel in line.chunks_exact(3) {
            file.push(pixel[2]);
            file.push(pixel[1]);
            file.push(pixel[0]);
        }
        // The row's padding to a four-byte boundary, which is nought to three
        // bytes and is part of the format rather than of the picture.
        file.resize(file.len() + (padded - stride), 0);
    }
    Ok(file)
}
