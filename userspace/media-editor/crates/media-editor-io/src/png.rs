// SPDX-License-Identifier: GPL-3.0-only
//! Writing a frame out as a PNG, so that a failure can be *looked at*.
//!
//! [`crate::sprw`] is the format this project keeps frames in. This is not
//! that. A golden render test compares a digest, and when a digest changes the
//! only thing it tells you is that something did — so
//! [`docs/VERIFICATION.md`] requires a reference frame stored beside every
//! golden hash, and until this existed there was nothing to store.
//!
//! # Deliberately minimal, and deliberately not the runtime's PNG
//!
//! Eight-bit greyscale, red-green-blue, and red-green-blue-alpha; no
//! interlacing, no palette, no ancillary chunks, and no compression at all.
//! The `IDAT` stream is a legal zlib stream made of DEFLATE *stored* blocks,
//! which is the compression method that does nothing — so the file is a little
//! larger than the pixels and every byte of it can be read by hand.
//!
//! [`docs/DEPENDENCIES.md`] plans to adopt the `png` crate for the runtime,
//! and this does not pre-empt that. Decoding somebody else's PNG is a
//! hostile-input problem with sixteen-bit channels, interlacing, palettes and
//! a dozen ancillary chunks; writing one small picture of our own for a human
//! to look at is a different job with none of those, and it needed no
//! dependency and no import gate to exist today.
//!
//! # PNG's alpha is straight, and this refuses to guess
//!
//! A PNG stores non-premultiplied colour. A premultiplied frame written out as
//! though it were straight is a picture darker than the one that was
//! rendered — worst exactly where coverage is lowest, which is the edge
//! somebody is usually looking at.
//!
//! Undoing the association here would be lossy, and lossy in a reference
//! capture is worse than anywhere else: the whole point is to see what
//! actually happened. So a premultiplied frame is refused by name, and
//! [`media_editor_render::unpremultiply`] is the caller's step to take and to
//! know the cost of.

use alloc::vec::Vec;

use media_editor_media::colour::AlphaState;
use media_editor_media::{Frame, PixelFormat};

use crate::status::{IoStatus, Result};

/// The eight bytes every PNG begins with.
pub const SIGNATURE: [u8; 8] = [0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A];

/// The most bytes one capture may occupy.
///
/// Sixty-four mebibytes. A reference frame is something a person opens, not
/// something a pipeline streams, and the bound exists so nothing can ask for
/// one the size of memory before anything checks (R-11.2).
pub const MAX_CAPTURE_BYTES: usize = 64 * 1024 * 1024;

/// The largest a stored DEFLATE block may be.
const STORED_BLOCK: usize = 65_535;

/// Write a frame as a PNG.
///
/// # Errors
///
/// [`IoStatus::PngFormatUnsupported`] for a format that is not eight-bit grey,
/// red-green-blue, or red-green-blue-alpha — a luma-chroma frame needs the
/// matrix taken out of it first, which is a named step and
/// [`media_editor_render::convert`] is where it lives.
/// [`IoStatus::PngPremultiplied`] for premultiplied coverage.
/// [`IoStatus::PayloadTooLarge`] past [`MAX_CAPTURE_BYTES`].
pub fn encode(frame: &Frame) -> Result<Vec<u8>> {
    let description = frame.description();
    let (colour_type, channels) = match description.format() {
        PixelFormat::Gray8 => (0_u8, 1_usize),
        PixelFormat::Rgb8 => (2, 3),
        PixelFormat::Rgba8 => (6, 4),
        _ => return Err(IoStatus::PngFormatUnsupported),
    };
    if description.alpha() == Some(AlphaState::Premultiplied) {
        return Err(IoStatus::PngPremultiplied);
    }

    let width = description.geometry().width();
    let height = description.geometry().height();
    let packed = frame.packed().map_err(IoStatus::Media)?;
    let row = usize::try_from(width)
        .ok()
        .and_then(|pixels| pixels.checked_mul(channels))
        .ok_or(IoStatus::PayloadTooLarge)?;

    // Each scanline is preceded by its filter type. Nought is "none", which is
    // the filter that does nothing — a reference capture is for reading, and a
    // filter would make the bytes on disc stop resembling the pixels.
    let mut raw = Vec::new();
    let rows = usize::try_from(height).map_err(|_| IoStatus::PayloadTooLarge)?;
    raw.try_reserve(rows.saturating_mul(row + 1))
        .map_err(|_| IoStatus::OutOfMemory)?;
    for index in 0..rows {
        raw.push(0);
        let start = index * row;
        raw.extend_from_slice(
            packed
                .get(start..start + row)
                .ok_or(IoStatus::TruncatedPayload)?,
        );
    }

    let mut out = Vec::new();
    out.try_reserve(SIGNATURE.len())
        .map_err(|_| IoStatus::OutOfMemory)?;
    out.extend_from_slice(&SIGNATURE);

    let mut header = Vec::new();
    header.try_reserve(13).map_err(|_| IoStatus::OutOfMemory)?;
    header.extend_from_slice(&width.to_be_bytes());
    header.extend_from_slice(&height.to_be_bytes());
    header.push(8);
    header.push(colour_type);
    header.push(0);
    header.push(0);
    header.push(0);
    chunk(&mut out, *b"IHDR", &header)?;
    chunk(&mut out, *b"IDAT", &zlib(&raw)?)?;
    chunk(&mut out, *b"IEND", &[])?;

    if out.len() > MAX_CAPTURE_BYTES {
        return Err(IoStatus::PayloadTooLarge);
    }
    Ok(out)
}

/// Append one chunk: its length, its name, its data, and their CRC.
///
/// The length covers the data alone and the CRC covers the name *and* the
/// data — which is the one thing about PNG's chunk layout that is easy to get
/// wrong, because the two spans are different and neither is the whole chunk.
fn chunk(out: &mut Vec<u8>, name: [u8; 4], data: &[u8]) -> Result<()> {
    let length = u32::try_from(data.len()).map_err(|_| IoStatus::PayloadTooLarge)?;
    out.try_reserve(data.len() + 12)
        .map_err(|_| IoStatus::OutOfMemory)?;
    out.extend_from_slice(&length.to_be_bytes());
    out.extend_from_slice(&name);
    out.extend_from_slice(data);

    let mut crc = Crc::new();
    crc.update(&name);
    crc.update(data);
    out.extend_from_slice(&crc.finish().to_be_bytes());
    Ok(())
}

/// A zlib stream holding uncompressed DEFLATE.
///
/// Two header bytes, then stored blocks, then Adler-32 of the *uncompressed*
/// data. `0x78 0x01` is a 32 KiB window with the fastest compression level,
/// and `0x7801` is a multiple of 31, which is the check the two bytes carry.
fn zlib(raw: &[u8]) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    out.try_reserve(raw.len() + raw.len() / STORED_BLOCK * 5 + 16)
        .map_err(|_| IoStatus::OutOfMemory)?;
    out.push(0x78);
    out.push(0x01);

    if raw.is_empty() {
        // An empty stream still needs a final block, or a reader waits for one
        // that never arrives.
        out.extend_from_slice(&[0x01, 0x00, 0x00, 0xFF, 0xFF]);
    }
    let mut written = 0;
    while written < raw.len() {
        let take = (raw.len() - written).min(STORED_BLOCK);
        let last = u8::from(written + take == raw.len());
        out.push(last);
        let length = u16::try_from(take).map_err(|_| IoStatus::PayloadTooLarge)?;
        out.extend_from_slice(&length.to_le_bytes());
        // The complement, which is the only redundancy a stored block carries.
        out.extend_from_slice(&(!length).to_le_bytes());
        out.extend_from_slice(&raw[written..written + take]);
        written += take;
    }

    out.extend_from_slice(&adler32(raw).to_be_bytes());
    Ok(out)
}

/// Adler-32, as zlib defines it.
fn adler32(bytes: &[u8]) -> u32 {
    let mut low = 1_u32;
    let mut high = 0_u32;
    for byte in bytes {
        low = (low + u32::from(*byte)) % 65_521;
        high = (high + low) % 65_521;
    }
    (high << 16) | low
}

/// CRC-32 as PNG uses it, computed a bit at a time.
///
/// No table. A table is 1 KiB of static data to save a few microseconds in
/// something that runs once per reference capture, and this project pays for
/// tables where they earn it rather than by habit.
struct Crc(u32);

impl Crc {
    /// A fresh accumulator.
    const fn new() -> Self {
        Self(0xFFFF_FFFF)
    }

    /// Take in some bytes.
    fn update(&mut self, bytes: &[u8]) {
        for byte in bytes {
            self.0 ^= u32::from(*byte);
            for _ in 0..8 {
                let carry = self.0 & 1;
                self.0 >>= 1;
                if carry != 0 {
                    // The reversed representation of the IEEE polynomial,
                    // which is the one every checksum in a PNG uses.
                    self.0 ^= 0xEDB8_8320;
                }
            }
        }
    }

    /// The finished checksum.
    const fn finish(self) -> u32 {
        self.0 ^ 0xFFFF_FFFF
    }
}
