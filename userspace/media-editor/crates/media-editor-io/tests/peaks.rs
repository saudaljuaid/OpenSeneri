// SPDX-License-Identifier: GPL-3.0-only
//! What a stored waveform summary is allowed to claim.
//!
//! The sweeps at the end are this crate's fuzzing until `cargo-fuzz` can run:
//! every single-byte change to a valid file must be refused, and so must every
//! prefix of one. A summary is read to draw with, so a file that decodes to
//! something plausible but wrong draws a waveform of a sound nobody has.

use media_editor_audio::overview::{MAX_BUCKET, MIN_BUCKET, Overview};
use media_editor_audio::{AudioBuffer, AudioStatus, SampleRate};
use media_editor_io::{IoStatus, peaks};

/// The block size the fixtures use.
const BASE: usize = MIN_BUCKET;

/// A repeatable pseudo-random channel, in 24-bit range.
fn noise(samples: usize, seed: u64) -> std::vec::Vec<i32> {
    let mut state = seed | 1;
    let mut held = std::vec::Vec::with_capacity(samples);
    for _ in 0..samples {
        state = state
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1_442_695_040_888_963_407);
        let value = i64::from((state >> 40) as u32 & 0x00FF_FFFF) - 8_388_608;
        held.push(i32::try_from(value).expect("a sample"));
    }
    held
}

/// A summary of two channels of noise, long enough to have several levels.
///
/// Eleven blocks rather than eight, so two levels have an odd count and the
/// file exercises a halving's remainder. A fixture whose dimensions divide
/// evenly tests the easy half of every function that divides.
fn sample() -> Overview {
    let channels = std::vec![noise(BASE * 11, 3), noise(BASE * 11, 11)];
    let buffer = AudioBuffer::new(SampleRate::Hz48000, channels).expect("a buffer");
    Overview::of(&buffer, BASE).expect("a summary")
}

#[test]
fn a_summary_survives_the_round_trip_exactly() {
    let overview = sample();
    let file = peaks::encode(&overview).expect("an encoding");
    let read = peaks::decode(&file).expect("a decoding");
    assert_eq!(read, overview);
    assert_eq!(read.digest(), overview.digest());

    // And writing what was read gives the same bytes, which is the property a
    // cache needs: a summary that re-encoded differently would invalidate
    // itself every time it was touched.
    assert_eq!(peaks::encode(&read).expect("an encoding"), file);
}

#[test]
fn the_header_is_the_length_it_says_it_is() {
    let file = peaks::encode(&sample()).expect("an encoding");
    assert_eq!(&file[..4], b"SPPK");
    assert_eq!(
        u16::from_le_bytes([file[4], file[5]]),
        peaks::FORMAT_VERSION
    );
    assert_eq!(file[8], 2, "48 kHz is tag two");
    assert_eq!(file[9], 2, "two channels");
    assert_eq!(
        u32::from_le_bytes([file[12], file[13], file[14], file[15]]),
        u32::try_from(BASE).expect("a base")
    );

    // Eleven blocks halve to six, three, two, one: 23 per channel, 46 in all.
    let blocks = (file.len() - peaks::HEADER_BYTES) / peaks::BUCKET_BYTES;
    assert_eq!(
        blocks, 46,
        "the payload is not the shape the header implies"
    );
}

#[test]
fn the_file_names_the_sound_it_summarises() {
    // The reason a peak file is worth keeping at all: staleness is visible.
    let channels = std::vec![noise(BASE * 11, 3), noise(BASE * 11, 11)];
    let buffer = AudioBuffer::new(SampleRate::Hz48000, channels).expect("a buffer");
    let file =
        peaks::encode(&Overview::of(&buffer, BASE).expect("a summary")).expect("an encoding");
    assert_eq!(&file[32..64], buffer.digest().bytes());
    assert_eq!(
        peaks::decode(&file).expect("a decoding").source(),
        buffer.digest()
    );

    // A different sound gives a different name, so the two can be told apart
    // without a clock.
    let other = std::vec![noise(BASE * 11, 4), noise(BASE * 11, 11)];
    let changed = AudioBuffer::new(SampleRate::Hz48000, other).expect("a buffer");
    let after =
        peaks::encode(&Overview::of(&changed, BASE).expect("a summary")).expect("an encoding");
    assert_ne!(&after[32..64], &file[32..64]);
}

#[test]
fn every_rate_this_build_carries_survives_a_round_trip() {
    // The tag table has two directions and they live beside each other in the
    // audio crate rather than one here and one there, so one test covers both
    // and adding a rate cannot leave a reader that does not know it.
    for rate in SampleRate::ALL {
        assert_eq!(SampleRate::from_tag(rate.tag()), Ok(rate));
        let buffer = AudioBuffer::new(rate, std::vec![noise(BASE * 5, 9)]).expect("a buffer");
        let overview = Overview::of(&buffer, BASE).expect("a summary");
        let read =
            peaks::decode(&peaks::encode(&overview).expect("an encoding")).expect("a decoding");
        assert_eq!(read.rate(), rate);
    }
    assert_eq!(
        SampleRate::from_tag(0),
        Err(AudioStatus::UnsupportedSampleRate)
    );
    assert_eq!(
        SampleRate::from_tag(5),
        Err(AudioStatus::UnsupportedSampleRate)
    );
}

#[test]
fn every_block_size_the_summary_allows_survives_a_round_trip() {
    let mut base = MIN_BUCKET;
    while base <= MAX_BUCKET {
        let buffer =
            AudioBuffer::new(SampleRate::Hz48000, std::vec![noise(base * 3, 7)]).expect("a buffer");
        let overview = Overview::of(&buffer, base).expect("a summary");
        let read =
            peaks::decode(&peaks::encode(&overview).expect("an encoding")).expect("a decoding");
        assert_eq!(read, overview, "a base of {base} did not survive");
        base *= 2;
    }
}

#[test]
fn a_length_that_is_not_a_whole_number_of_blocks_survives() {
    // The last block is short, its count is derived rather than stored, and
    // the reader has to arrive at the same shape the writer did — from the
    // header alone.
    for extra in [1, 2, BASE - 1] {
        let buffer = AudioBuffer::new(SampleRate::Hz48000, std::vec![noise(BASE * 7 + extra, 2)])
            .expect("a buffer");
        let overview = Overview::of(&buffer, BASE).expect("a summary");
        let read =
            peaks::decode(&peaks::encode(&overview).expect("an encoding")).expect("a decoding");
        assert_eq!(read, overview, "a tail of {extra} samples did not survive");
    }
}

#[test]
fn something_that_is_not_a_summary_is_refused_by_name() {
    let mut file = peaks::encode(&sample()).expect("an encoding");
    file[0] = b'X';
    assert_eq!(peaks::decode(&file).map(|_| ()), Err(IoStatus::NotASummary));

    assert_eq!(
        peaks::decode(b"SPPK").map(|_| ()),
        Err(IoStatus::TruncatedHeader)
    );
    assert_eq!(
        peaks::decode(&[]).map(|_| ()),
        Err(IoStatus::TruncatedHeader)
    );
}

#[test]
fn a_version_this_build_does_not_write_is_refused_by_number() {
    // Not "unsupported" alone: the number is in the refusal, because the first
    // thing anyone asks on seeing one is which version the file claims to be.
    let mut file = peaks::encode(&sample()).expect("an encoding");
    file[4] = 9;
    assert_eq!(
        peaks::decode(&file).map(|_| ()),
        Err(IoStatus::UnsupportedVersion(9))
    );
}

#[test]
fn a_payload_that_is_not_the_length_the_header_implies_is_refused() {
    // The number of levels is not in the header, so a payload of the wrong
    // length is the only way the disagreement can show — and it must show,
    // rather than the reader taking what it can and stopping.
    let file = peaks::encode(&sample()).expect("an encoding");

    let mut short = file.clone();
    short.truncate(file.len() - peaks::BUCKET_BYTES);
    assert_eq!(
        peaks::decode(&short).map(|_| ()),
        Err(IoStatus::DigestMismatch),
        "a short payload is caught by the digest before the shape"
    );

    // With the digest repaired, the shape is what refuses — which is the case
    // worth having, because it is the one a deliberate edit produces.
    let mended = mend(&short);
    assert_eq!(
        peaks::decode(&mended).map(|_| ()),
        Err(IoStatus::TruncatedPayload)
    );

    let mut long = file.clone();
    long.extend_from_slice(&[0; peaks::BUCKET_BYTES]);
    let mended = mend(&long);
    assert_eq!(
        peaks::decode(&mended).map(|_| ()),
        Err(IoStatus::TrailingBytes),
        "a summary's length follows from its header, so extra blocks are a \
         disagreement rather than padding"
    );
}

/// Recompute a file's digest so that a deliberate edit reaches the checks past
/// it.
///
/// Without this every test of the shape checks would be a test of the digest,
/// which is a different check and already has its own.
fn mend(file: &[u8]) -> std::vec::Vec<u8> {
    let mut held = file.to_vec();
    let mut hasher = media_editor_core::Sha256::new();
    hasher.update(&held[..peaks::DESCRIBED_BYTES]);
    hasher.update(&held[peaks::HEADER_BYTES..]);
    held[peaks::DESCRIBED_BYTES..peaks::HEADER_BYTES].copy_from_slice(hasher.finish().bytes());
    held
}

#[test]
fn a_header_that_describes_no_summary_is_refused() {
    let file = peaks::encode(&sample()).expect("an encoding");

    // A block size that is not a power of two.
    let mut odd = file.clone();
    odd[12..16].copy_from_slice(&100_u32.to_le_bytes());
    assert_eq!(
        peaks::decode(&mend(&odd)).map(|_| ()),
        Err(IoStatus::Sound(AudioStatus::BucketSizeUnsupported))
    );

    // A block size of nought, which would make the first division a division
    // by nought rather than a refusal.
    let mut nought = file.clone();
    nought[12..16].copy_from_slice(&0_u32.to_le_bytes());
    assert_eq!(
        peaks::decode(&mend(&nought)).map(|_| ()),
        Err(IoStatus::Sound(AudioStatus::BucketSizeUnsupported))
    );

    // No samples, which is a summary of nothing.
    let mut empty = file.clone();
    empty[16..24].copy_from_slice(&0_u64.to_le_bytes());
    assert_eq!(
        peaks::decode(&mend(&empty)).map(|_| ()),
        Err(IoStatus::Sound(AudioStatus::BufferTooShort))
    );

    // No channels.
    let mut none = file.clone();
    none[9] = 0;
    assert_eq!(
        peaks::decode(&mend(&none)).map(|_| ()),
        Err(IoStatus::Sound(AudioStatus::ChannelCountUnsupported))
    );

    // More samples than this build will hold, which must be refused before
    // anything tries to reserve room for the blocks.
    let mut vast = file.clone();
    vast[16..24].copy_from_slice(&u64::MAX.to_le_bytes());
    assert!(peaks::decode(&mend(&vast)).is_err());

    // A rate no tag names.
    let mut rate = file.clone();
    rate[8] = 9;
    assert_eq!(
        peaks::decode(&mend(&rate)).map(|_| ()),
        Err(IoStatus::Sound(AudioStatus::UnsupportedSampleRate))
    );
}

#[test]
fn a_reserved_field_that_is_set_is_refused() {
    // A reserved field is how a later version says something, so a reader that
    // ignored one would read a file it does not understand as one it does.
    let file = peaks::encode(&sample()).expect("an encoding");
    for index in [6, 7, 10, 11, 24, 25, 26, 27, 28, 29, 30, 31] {
        let mut mutated = file.clone();
        mutated[index] = 1;
        assert_eq!(
            peaks::decode(&mend(&mutated)).map(|_| ()),
            Err(IoStatus::ReservedFieldSet),
            "reserved byte {index} was ignored"
        );
    }
}

#[test]
fn a_block_that_describes_no_samples_is_refused() {
    // The lowest above the highest, straight out of the payload.
    let file = peaks::encode(&sample()).expect("an encoding");
    let mut mutated = file.clone();
    let block = peaks::HEADER_BYTES;
    mutated[block..block + 4].copy_from_slice(&8_388_607_i32.to_le_bytes());
    mutated[block + 4..block + 8].copy_from_slice(&(-8_388_608_i32).to_le_bytes());
    assert_eq!(
        peaks::decode(&mend(&mutated)).map(|_| ()),
        Err(IoStatus::Sound(AudioStatus::BucketNotOrdered))
    );

    // Energy no samples could carry.
    let mut wild = file.clone();
    wild[block + 8..block + 16].copy_from_slice(&i64::MAX.to_le_bytes());
    assert_eq!(
        peaks::decode(&mend(&wild)).map(|_| ()),
        Err(IoStatus::Sound(AudioStatus::BucketNotPossible))
    );
}

#[test]
fn every_single_byte_change_is_refused() {
    // The digest covers the header as well as the blocks, so this passes for a
    // reason: without that, a flipped bit in the block size would leave every
    // block in the file describing a different span of time, silently.
    let file = peaks::encode(&sample()).expect("an encoding");
    let mut checked = 0;
    for index in 0..file.len() {
        for replacement in [0x00_u8, 0x01, 0x55, 0xFF] {
            if file[index] == replacement {
                continue;
            }
            let mut mutated = file.clone();
            mutated[index] = replacement;
            assert!(
                peaks::decode(&mutated).is_err(),
                "byte {index} changed to {replacement:#04x} was accepted"
            );
            checked += 1;
        }
    }
    assert!(checked > 1000, "the sweep covered only {checked} mutations");
}

#[test]
fn every_prefix_is_refused() {
    let file = peaks::encode(&sample()).expect("an encoding");
    for length in 0..file.len() {
        assert!(
            peaks::decode(&file[..length]).is_err(),
            "a summary truncated to {length} bytes was accepted"
        );
    }
    assert!(peaks::decode(&file).is_ok());
}
