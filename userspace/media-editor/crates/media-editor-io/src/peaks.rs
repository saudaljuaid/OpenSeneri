// SPDX-License-Identifier: GPL-3.0-only
//! `SPPK`: the waveform a timeline draws, as a file.
//!
//! ```text
//! offset  size  field
//! 0       4     magic, "SPPK"
//! 4       2     format version, little-endian
//! 6       2     reserved, must be zero
//! 8       1     sample rate
//! 9       1     channel count
//! 10      2     reserved, must be zero
//! 12      4     samples per block at level zero
//! 16      8     samples per channel in the sound summarised
//! 24      8     reserved, must be zero
//! 32      32    SHA-256 of the sound this summarises
//! 64      32    SHA-256 of bytes 0..64 followed by the payload
//! 96      N     the blocks
//! ```
//!
//! Each block stores signed 32-bit minimum and maximum samples followed by a
//! signed 64-bit mean square. Level zero comes first; each following level has
//! half as many blocks, rounded up. Blocks are grouped by channel within a
//! level.
//!
//! Level counts are derived from the base block size and sample count. Readers
//! reject any payload whose exact derived length does not match. The source
//! digest prevents a summary from being used with different audio.

use alloc::vec::Vec;

use media_editor_audio::overview::{Bucket, Overview};
use media_editor_audio::{MAX_CHANNELS, SampleRate};
use media_editor_core::Digest;

use crate::bytes::{Reader, Writer};
use crate::status::{IoStatus, Result};

/// The four bytes every summary begins with.
pub const MAGIC: [u8; 4] = *b"SPPK";

/// The format this build writes.
pub const FORMAT_VERSION: u16 = 1;

/// How long the fixed header is.
pub const HEADER_BYTES: usize = 96;

/// How much of the header the digest covers: everything before the digest
/// field itself.
pub const DESCRIBED_BYTES: usize = 64;

/// How many bytes one block occupies.
pub const BUCKET_BYTES: usize = 16;

/// The most bytes one summary file may occupy.
///
/// A hundred and twenty-eight mebibytes. The worst case is real rather than
/// theoretical: sixteen channels of the longest buffer this build accepts,
/// summarised at the finest block size, is about ninety-six — so the bound is
/// above what the audio crate can hand over and below what a mistake could ask
/// for (R-11.2).
pub const MAX_SUMMARY_BYTES: usize = 128 * 1024 * 1024;

/// Write a summary.
///
/// # Errors
///
/// [`IoStatus::PayloadTooLarge`] past [`MAX_SUMMARY_BYTES`], and
/// [`IoStatus::OutOfMemory`].
pub fn encode(overview: &Overview) -> Result<Vec<u8>> {
    let mut payload = Writer::new(MAX_SUMMARY_BYTES);
    for level in 0..overview.level_count() {
        for channel in 0..overview.channel_count() {
            for bucket in overview.buckets(level, channel)? {
                payload.i32(bucket.minimum())?;
                payload.i32(bucket.maximum())?;
                payload.i64(bucket.mean_square())?;
            }
        }
    }
    let payload = payload.finish();

    let mut head = Writer::new(DESCRIBED_BYTES);
    head.bytes(&MAGIC)?;
    head.u16(FORMAT_VERSION)?;
    head.u16(0)?;
    head.u8(overview.rate().tag())?;
    head.u8(u8::try_from(overview.channel_count()).map_err(|_| IoStatus::TooMany)?)?;
    head.u16(0)?;
    head.u32(u32::try_from(overview.base()).map_err(|_| IoStatus::TooMany)?)?;
    head.u64(u64::try_from(overview.len()).map_err(|_| IoStatus::TooMany)?)?;
    head.u64(0)?;
    head.bytes(overview.source().bytes())?;
    let head = head.finish();

    let mut file = Writer::new(HEADER_BYTES + MAX_SUMMARY_BYTES);
    file.bytes(&head)?;
    file.bytes(digest_of(&head, &payload).bytes())?;
    file.bytes(&payload)?;
    Ok(file.finish())
}

/// Read a summary.
///
/// # Errors
///
/// Any [`IoStatus`]. Nothing is returned on a refusal.
pub fn decode(file: &[u8]) -> Result<Overview> {
    if file.len() < HEADER_BYTES {
        return Err(IoStatus::TruncatedHeader);
    }
    let mut header = Reader::new(&file[..DESCRIBED_BYTES]);
    if header.take(4)? != MAGIC {
        return Err(IoStatus::NotASummary);
    }
    let version = header.u16()?;
    if version != FORMAT_VERSION {
        return Err(IoStatus::UnsupportedVersion(version));
    }
    if header.u16()? != 0 {
        return Err(IoStatus::ReservedFieldSet);
    }
    let rate = SampleRate::from_tag(header.u8()?)?;
    let channels = usize::from(header.u8()?);
    if channels == 0 || channels > MAX_CHANNELS {
        return Err(IoStatus::Sound(
            media_editor_audio::AudioStatus::ChannelCountUnsupported,
        ));
    }
    if header.u16()? != 0 {
        return Err(IoStatus::ReservedFieldSet);
    }
    let base = usize::try_from(header.u32()?).map_err(|_| IoStatus::TooMany)?;
    let length = usize::try_from(header.u64()?).map_err(|_| IoStatus::TooMany)?;
    if header.u64()? != 0 {
        return Err(IoStatus::ReservedFieldSet);
    }
    let source = Digest::new(header.digest_bytes()?);

    // The digest before the shape, because the shape is computed from header
    // fields and a flipped bit in the block size would otherwise send the
    // reader looking for a payload of the wrong length and report *that*. The
    // honest finding is that the file is not the file it says it is.
    let stored = Digest::new(Reader::new(&file[DESCRIBED_BYTES..HEADER_BYTES]).digest_bytes()?);
    let payload = &file[HEADER_BYTES..];
    if digest_of(&file[..DESCRIBED_BYTES], payload) != stored {
        return Err(IoStatus::DigestMismatch);
    }

    let shape = shape_of(length, base, channels)?;
    let wanted = shape
        .iter()
        .sum::<usize>()
        .checked_mul(BUCKET_BYTES)
        .ok_or(IoStatus::PayloadTooLarge)?;
    if payload.len() < wanted {
        return Err(IoStatus::TruncatedPayload);
    }
    if payload.len() > wanted {
        // Not a payload that is merely long: a summary's length follows from
        // its header, so a longer one is a disagreement rather than padding.
        return Err(IoStatus::TrailingBytes);
    }

    let mut reader = Reader::new(payload);
    let mut levels = Vec::new();
    levels
        .try_reserve(shape.len())
        .map_err(|_| IoStatus::OutOfMemory)?;
    for count in shape {
        let mut level = Vec::new();
        level
            .try_reserve(count)
            .map_err(|_| IoStatus::OutOfMemory)?;
        for _ in 0..count {
            let minimum = reader.i32()?;
            let maximum = reader.i32()?;
            let mean = reader.i64()?;
            level.push(Bucket::new(minimum, maximum, mean)?);
        }
        levels.push(level);
    }
    Ok(Overview::assemble(
        rate, channels, length, base, source, levels,
    )?)
}

/// How many blocks each level holds, all channels together.
///
/// Computed rather than read, which is the whole reason it is not in the
/// header. The base is checked here because the shape is meaningless without
/// it: a block size of nought would make the first division a division by
/// nought, and one that is not a power of two would make the levels stop
/// tiling each other.
fn shape_of(length: usize, base: usize, channels: usize) -> Result<Vec<usize>> {
    if length == 0 {
        return Err(IoStatus::Sound(
            media_editor_audio::AudioStatus::BufferTooShort,
        ));
    }
    media_editor_audio::overview::check_base(base)?;
    if length > media_editor_audio::MAX_SAMPLES {
        return Err(IoStatus::Sound(
            media_editor_audio::AudioStatus::BufferTooLong,
        ));
    }

    let mut shape = Vec::new();
    let mut count = length.div_ceil(base);
    loop {
        shape.try_reserve(1).map_err(|_| IoStatus::OutOfMemory)?;
        shape.push(count * channels);
        if count <= 1 {
            break;
        }
        count = count.div_ceil(media_editor_audio::overview::FANOUT);
    }
    Ok(shape)
}

/// The digest a summary carries: its description and its blocks together.
///
/// The header as well as the blocks, and the byte sweep says exactly what that
/// buys. With the header excluded, thirty-three of its bytes become
/// undetectably editable: byte 8, the sample rate, so a 48 kHz summary reads as
/// 44.1 and every block covers a different span of time — and bytes 32 to 63,
/// which are the digest of the sound this summarises.
///
/// That second one is the sharp case. It is the field whose entire purpose is
/// to let a reader see that a summary is stale, and without header coverage it
/// is the field a corruption can rewrite in silence. A staleness check that
/// can itself go stale is not a check.
fn digest_of(head: &[u8], payload: &[u8]) -> Digest {
    let mut hasher = media_editor_core::Sha256::new();
    hasher.update(head);
    hasher.update(payload);
    hasher.finish()
}
