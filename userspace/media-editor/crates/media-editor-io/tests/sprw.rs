// SPDX-License-Identifier: GPL-3.0-only
//! The uncompressed mezzanine: what it accepts, and everything it refuses.

use media_editor_core::{Rational, Timebase};
use media_editor_io::IoStatus;
use media_editor_io::sprw::{self, FORMAT_VERSION, HEADER_BYTES, MAGIC, Reel};
use media_editor_media::{
    AlphaState, ChromaSiting, ColourDescription, Frame, FrameDescription, Geometry, PixelFormat,
    TestPattern,
};

/// What a format's alpha tag must be for a description to be accepted at all.
///
/// A format either carries an alpha channel or it does not, and a description
/// must say which association applies exactly when it does.
fn alpha_for(format: PixelFormat) -> Option<AlphaState> {
    if format.has_alpha() {
        Some(AlphaState::Straight)
    } else {
        None
    }
}

fn description(width: u32, height: u32, format: PixelFormat) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(width, height).expect("a geometry"),
        format,
        ColourDescription::srgb_full(),
        None,
        alpha_for(format),
    )
    .expect("a description")
}

/// A short reel of test patterns.
fn sample() -> Reel {
    let description = description(16, 9, PixelFormat::Rgb8);
    let frames = std::vec![
        TestPattern::Bars.render(description).expect("a frame"),
        TestPattern::Ramp.render(description).expect("a frame"),
        TestPattern::Flat { value: 77 }
            .render(description)
            .expect("a frame"),
    ];
    Reel::new(Timebase::NTSC_FILM, frames).expect("a reel")
}

#[test]
fn a_reel_survives_a_round_trip() {
    let reel = sample();
    let file = sprw::encode(&reel).expect("an encoding");
    let decoded = sprw::decode(&file).expect("a decoding");
    assert_eq!(decoded, reel);
    assert_eq!(decoded.timebase(), Timebase::NTSC_FILM);
    assert_eq!(decoded.len(), 3);
    for (before, after) in reel.frames().iter().zip(decoded.frames()) {
        assert_eq!(before.digest(), after.digest(), "frame for frame");
    }
}

#[test]
fn the_file_is_the_layout_the_module_comment_states() {
    // The format written out by hand, field by field, from the table in
    // `sprw`'s own module comment -- not read back out of the encoder. A test
    // that asked the code what the code produced would agree with any layout
    // at all, which is exactly what a format version bump must not be checked
    // by.
    let reel = sample();
    let file = sprw::encode(&reel).expect("an encoding");

    let mut expected = std::vec::Vec::new();
    expected.extend_from_slice(b"SPRW"); //  0  magic
    expected.extend_from_slice(&5_u16.to_le_bytes()); //  4  version
    expected.extend_from_slice(&0_u16.to_le_bytes()); //  6  reserved
    expected.extend_from_slice(&16_u32.to_le_bytes()); //  8  width
    expected.extend_from_slice(&9_u32.to_le_bytes()); // 12  height
    // The tags, from `sprw`'s own tables, for the `srgb_full` description the
    // fixture is built with. Written out rather than fetched: the first draft
    // of this test guessed two of them from their names and was wrong about
    // both -- sRGB's matrix is *Identity*, because sRGB is already red, green
    // and blue and there is nothing to un-mix.
    expected.push(2); // 16  format: Rgb8
    expected.push(1); // 17  primaries: BT.709
    expected.push(2); // 18  transfer: sRGB
    expected.push(1); // 19  matrix: identity
    expected.push(2); // 20  range: full
    expected.push(0); // 21  siting: none
    expected.push(0); // 22  alpha: a format that carries none
    expected.push(0); // 23  reserved
    expected.extend_from_slice(&1_i64.to_le_bytes()); // 24  aspect numerator
    expected.extend_from_slice(&1_i64.to_le_bytes()); // 32  aspect denominator
    expected.extend_from_slice(&24_000_i64.to_le_bytes()); // 40  rate numerator
    expected.extend_from_slice(&1001_i64.to_le_bytes()); // 48  rate denominator
    expected.extend_from_slice(&3_u64.to_le_bytes()); // 56  frame count
    expected.push(0); // 64  sample rate: a reel with no sound
    expected.push(0); // 65  channel count: the same
    expected.extend_from_slice(&[0; 6]); // 66  reserved
    expected.extend_from_slice(&0_u64.to_le_bytes()); // 72  sample count
    expected.extend_from_slice(&0_u32.to_le_bytes()); // 80  caption count
    expected.extend_from_slice(&0_u32.to_le_bytes()); // 84  caption bytes
    assert_eq!(expected.len(), HEADER_BYTES, "the header is eighty-eight");

    // 88  the frames, tightly packed, in order.
    for frame in reel.frames() {
        expected.extend_from_slice(&frame.to_packed().expect("bytes"));
    }
    let payload_end = expected.len();
    assert_eq!(payload_end, HEADER_BYTES + 3 * 16 * 9 * 3);

    // 88+N  SHA-256 of bytes 0..88 followed by the payload. Hashed here in one
    // pass over one slice, where the encoder hashes a header and a payload
    // separately -- so the two agree about what "followed by" means.
    expected.extend_from_slice(media_editor_core::Digest::of(&expected).bytes());
    assert_eq!(expected.len(), payload_end + sprw::TRAILER_BYTES);

    assert_eq!(file, expected, "the encoder and the stated layout disagree");
}

#[test]
fn the_encoding_is_canonical() {
    let first = sprw::encode(&sample()).expect("an encoding");
    let second = sprw::encode(&sprw::decode(&first).expect("a decoding")).expect("an encoding");
    assert_eq!(first, second);
}

#[test]
fn the_file_is_exactly_as_large_as_it_should_be() {
    let reel = sample();
    let file = sprw::encode(&reel).expect("an encoding");
    let frame_bytes = reel.description().packed_bytes().expect("a size");
    assert_eq!(
        file.len(),
        HEADER_BYTES + frame_bytes * reel.len() + sprw::TRAILER_BYTES
    );
    assert_eq!(&file[..4], &MAGIC);
    // The header is eighty-eight and the trailer is thirty-two, worked out
    // from the layout in the module comment rather than read back out of the
    // code: 4 + 2 + 2 + 4 + 4 + 7 + 1 + 8 + 8 + 8 + 8 + 8 = 64 for the
    // picture, 1 + 1 + 6 + 8 = 16 for the sound, 4 + 4 = 8 for the transcript,
    // and one SHA-256 is 32. Version two was 96 + N with the digest at offset
    // 64; version three was 64 + N + 32, the same length; version four added
    // sixteen and version five eight more, and every reel pays for both
    // whether it has sound or words or neither.
    assert_eq!(HEADER_BYTES, 88);
    assert_eq!(sprw::TRAILER_BYTES, 32);
}

#[test]
fn every_format_and_description_round_trips() {
    let cases: [(PixelFormat, Option<ChromaSiting>, ColourDescription); 4] = [
        (PixelFormat::Rgba8, None, ColourDescription::srgb_full()),
        (PixelFormat::Gray8, None, ColourDescription::srgb_full()),
        (
            PixelFormat::Yuv420p8,
            Some(ChromaSiting::Centre),
            ColourDescription::bt709_limited(),
        ),
        (
            PixelFormat::Yuv444p8,
            None,
            ColourDescription::bt709_limited(),
        ),
    ];
    for (format, siting, colour) in cases {
        let described = FrameDescription::new(
            Geometry::new(8, 6).expect("a geometry"),
            format,
            colour,
            siting,
            alpha_for(format),
            Rational::new(16, 11).expect("a ratio"),
        )
        .expect("a description");
        let frame = Frame::blank(described).expect("a frame");
        let reel = Reel::new(Timebase::PAL_25, std::vec![frame]).expect("a reel");
        let file = sprw::encode(&reel).expect("an encoding");
        assert_eq!(sprw::decode(&file).expect("a decoding"), reel, "{format:?}");
    }
}

#[test]
fn frames_described_differently_are_not_one_reel() {
    let first = Frame::blank(description(8, 8, PixelFormat::Gray8)).expect("a frame");
    let second = Frame::blank(description(8, 4, PixelFormat::Gray8)).expect("a frame");
    assert_eq!(
        Reel::new(Timebase::PAL_25, std::vec![first, second]),
        Err(IoStatus::ReelDescriptionMismatch),
        "a file whose frames change shape halfway through is two files"
    );
}

#[test]
fn a_reel_with_no_frames_cannot_be_built_or_read() {
    assert_eq!(
        Reel::new(Timebase::PAL_25, std::vec![]),
        Err(IoStatus::EmptyReel)
    );

    let mut file = sprw::encode(&sample()).expect("an encoding");
    file[56..64].copy_from_slice(&0_u64.to_le_bytes());
    assert_eq!(sprw::decode(&file), Err(IoStatus::EmptyReel));
}

#[test]
fn a_file_that_is_not_a_reel_is_refused() {
    assert_eq!(sprw::decode(b""), Err(IoStatus::TruncatedHeader));
    assert_eq!(
        sprw::decode(&[0; HEADER_BYTES - 1]),
        Err(IoStatus::TruncatedHeader)
    );

    let mut file = sprw::encode(&sample()).expect("an encoding");
    file[1] = b'X';
    assert_eq!(sprw::decode(&file), Err(IoStatus::NotAReel));
}

#[test]
fn a_reel_that_ends_before_its_digest_is_refused_without_hashing_anything() {
    // The case version three exists to make cheap. A write interrupted after
    // the last sample and before the trailer leaves a file whose header is
    // sound, whose payload is complete, and which is thirty-two bytes short --
    // and the refusal is a subtraction rather than a rehash of everything that
    // did arrive.
    let file = sprw::encode(&sample()).expect("an encoding");
    let cut = &file[..file.len() - sprw::TRAILER_BYTES];
    assert_eq!(sprw::decode(cut), Err(IoStatus::TruncatedPayload));
    // One byte of the trailer is missing rather than all of it: the same
    // answer, because a partial digest is not a digest.
    assert_eq!(
        sprw::decode(&file[..file.len() - 1]),
        Err(IoStatus::TruncatedPayload)
    );
    // And the streaming reader agrees, in sixty-four bytes, having read
    // neither the payload nor the trailer.
    assert_eq!(
        sprw::Spool::open(&cut),
        Err(IoStatus::TruncatedPayload),
        "the cheap door and the expensive one give the same answer"
    );
}

#[test]
fn a_future_version_is_refused_by_number() {
    let mut file = sprw::encode(&sample()).expect("an encoding");
    file[4..6].copy_from_slice(&(FORMAT_VERSION + 1).to_le_bytes());
    assert_eq!(
        sprw::decode(&file),
        Err(IoStatus::UnsupportedVersion(FORMAT_VERSION + 1))
    );
}

#[test]
fn an_undefined_tag_is_refused_rather_than_defaulted() {
    let file = sprw::encode(&sample()).expect("an encoding");
    for (offset, name) in [
        (16_usize, "pixel format"),
        (17, "primaries"),
        (18, "transfer"),
        (19, "matrix"),
        (20, "range"),
        (21, "siting"),
    ] {
        let mut mutated = file.clone();
        mutated[offset] = 200;
        assert!(
            sprw::decode(&mutated).is_err(),
            "an undefined {name} tag was accepted"
        );
    }
}

#[test]
fn a_frame_count_that_disagrees_with_the_payload_is_refused() {
    let file = sprw::encode(&sample()).expect("an encoding");

    let mut more = file.clone();
    more[56..64].copy_from_slice(&4_u64.to_le_bytes());
    assert_eq!(sprw::decode(&more), Err(IoStatus::TruncatedPayload));

    let mut fewer = file.clone();
    fewer[56..64].copy_from_slice(&2_u64.to_le_bytes());
    assert_eq!(sprw::decode(&fewer), Err(IoStatus::TrailingBytes));
}

#[test]
fn a_frame_count_no_payload_could_hold_is_refused_before_anything_is_allocated() {
    // The hostile case: a header claiming four billion frames. It must be
    // refused by the bound, not by the allocator running out (R-11.2).
    let mut file = sprw::encode(&sample()).expect("an encoding");
    file[56..64].copy_from_slice(&u64::MAX.to_le_bytes());
    assert_eq!(sprw::decode(&file), Err(IoStatus::TooMany));

    file[56..64].copy_from_slice(&100_000_u64.to_le_bytes());
    assert_eq!(sprw::decode(&file), Err(IoStatus::TooMany));
}

#[test]
fn a_geometry_the_format_cannot_express_is_refused() {
    // 4:2:0 halves both dimensions, so an odd width has no chroma plane.
    let mut file = sprw::encode(&sample()).expect("an encoding");
    file[16] = 4; // claim 4:2:0
    file[21] = 1; // and a siting, so that is not what fails
    assert!(sprw::decode(&file).is_err());
}

#[test]
fn every_single_byte_change_is_refused() {
    let file = sprw::encode(&sample()).expect("an encoding");
    for index in 0..file.len() {
        for replacement in [0x00_u8, 0x01, 0x55, 0xFF] {
            if file[index] == replacement {
                continue;
            }
            let mut mutated = file.clone();
            mutated[index] = replacement;
            assert!(
                sprw::decode(&mutated).is_err(),
                "byte {index} changed to {replacement:#04x} was accepted"
            );
        }
    }
}

#[test]
fn every_prefix_is_refused() {
    let file = sprw::encode(&sample()).expect("an encoding");
    for length in 0..file.len() {
        assert!(
            sprw::decode(&file[..length]).is_err(),
            "a reel truncated to {length} bytes was accepted"
        );
    }
    assert!(sprw::decode(&file).is_ok());
}

/// xorshift64*, so a failure is a seed rather than an anecdote.
struct Generator(u64);

impl Generator {
    fn next(&mut self) -> u64 {
        let mut state = self.0;
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        self.0 = state;
        state.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }
}

#[test]
fn garbage_is_refused_and_never_anything_worse() {
    let mut generator = Generator(0xFEED_FACE_CAFE_BEEF);
    for _ in 0..2000 {
        let length = usize::try_from(generator.next() % 400).unwrap_or(0);
        let mut bytes: std::vec::Vec<u8> = std::vec::Vec::with_capacity(length);
        for _ in 0..length {
            bytes.push(u8::try_from(generator.next() & 0xFF).unwrap_or(0));
        }
        assert!(sprw::decode(&bytes).is_err(), "random bytes were accepted");
    }
}

#[test]
fn the_alpha_association_survives_the_file() {
    // A mezzanine that dropped this would hand the next stage samples whose
    // meaning it had to guess, and guessing wrong is a dark fringe.
    for state in [AlphaState::Straight, AlphaState::Premultiplied] {
        let described = FrameDescription::square(
            Geometry::new(8, 6).expect("a geometry"),
            PixelFormat::Rgba8,
            ColourDescription::srgb_full(),
            None,
            Some(state),
        )
        .expect("a description");
        let reel = Reel::new(
            Timebase::PAL_25,
            std::vec![Frame::blank(described).expect("a frame")],
        )
        .expect("a reel");
        let file = sprw::encode(&reel).expect("an encoding");
        assert_eq!(file[22], if state == AlphaState::Straight { 1 } else { 2 });
        let back = sprw::decode(&file).expect("a decoding");
        assert_eq!(back.description().alpha(), Some(state));
        assert_eq!(back, reel);
    }
}

#[test]
fn changing_the_alpha_byte_changes_the_file() {
    // The header is digested, so this must be caught rather than silently
    // reinterpreted — the gap the byte sweep found once already.
    let described = FrameDescription::square(
        Geometry::new(8, 6).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Straight),
    )
    .expect("a description");
    let reel = Reel::new(
        Timebase::PAL_25,
        std::vec![Frame::blank(described).expect("a frame")],
    )
    .expect("a reel");
    let mut file = sprw::encode(&reel).expect("an encoding");
    file[22] = 2;
    assert_eq!(
        sprw::decode(&file),
        Err(IoStatus::DigestMismatch),
        "premultiplied is not what this file says it holds"
    );
}

#[test]
fn an_unknown_alpha_tag_is_refused() {
    // The header is parsed before the digest is checked, so this is the tag
    // reader refusing rather than the digest catching it. Both must hold, and
    // a test that would accept either proves neither.
    let mut file = sprw::encode(&sample()).expect("an encoding");
    file[22] = 3;
    assert_eq!(sprw::decode(&file), Err(IoStatus::UnknownColourTag(3)));
}

#[test]
fn a_version_one_file_is_not_read_as_version_two() {
    // Version one had two reserved bytes where version two keeps the alpha
    // association. Reading one as the other would silently call every frame
    // straight, so the version number is checked before any field is.
    let mut file = sprw::encode(&sample()).expect("an encoding");
    file[4] = 1;
    file[5] = 0;
    assert_eq!(sprw::decode(&file), Err(IoStatus::UnsupportedVersion(1)));
}

#[test]
fn an_alpha_tag_a_format_cannot_carry_is_refused_even_with_a_sound_digest() {
    // A corrupt file is caught by the digest. This is the other case: a file
    // that is internally consistent and still describes something that is not
    // a frame — an alpha association on a format with no alpha channel. The
    // digest is recomputed so that it cannot be what does the refusing.
    let file = sprw::encode(&sample()).expect("an encoding");
    let mut forged = file.clone();
    assert_eq!(forged[16], 2, "the sample reel is Rgb8, which has no alpha");
    forged[22] = 1;
    let end = forged.len() - sprw::TRAILER_BYTES;
    let mut hasher = media_editor_core::Sha256::new();
    hasher.update(&forged[..HEADER_BYTES]);
    hasher.update(&forged[HEADER_BYTES..end]);
    let digest = hasher.finish();
    forged[end..].copy_from_slice(digest.bytes());

    assert_ne!(forged, file, "the forgery must differ from the original");
    assert_eq!(
        sprw::decode(&forged),
        Err(IoStatus::Media(
            media_editor_media::MediaStatus::AlphaMismatch
        )),
        "a sound digest over a description that cannot exist is still refused"
    );
}

/// A file with a byte changed and its digest made to agree again.
///
/// So that the refusal under test is the one being tested rather than the
/// digest. Six controls in this project's history passed because no test built
/// a hostile *file* — only round trips — and this is the shape that answer
/// took.
fn resealed(file: &[u8], edit: impl FnOnce(&mut std::vec::Vec<u8>)) -> std::vec::Vec<u8> {
    let mut forged = file.to_vec();
    edit(&mut forged);
    let end = forged.len() - sprw::TRAILER_BYTES;
    let mut hasher = media_editor_core::Sha256::new();
    hasher.update(&forged[..end]);
    let digest = hasher.finish();
    forged[end..].copy_from_slice(digest.bytes());
    forged
}

#[test]
fn a_header_that_half_declares_sound_is_refused() {
    // A rate with no channels, or channels at no rate, is a header that
    // disagrees with itself — and it is *not* caught by the digest, because a
    // file can be internally consistent and still describe something that is
    // not a reel. The forgery is resealed so that the digest cannot be what
    // does the refusing.
    let file = sprw::encode(&sample()).expect("an encoding");
    assert_eq!(file[64], 0, "the sample reel has no sound");
    assert_eq!(file[65], 0);

    // A rate, and nothing to play it in.
    let rate_only = resealed(&file, |forged| forged[64] = 2);
    assert_eq!(sprw::decode(&rate_only), Err(IoStatus::SoundNotDeclared));
    // Channels, at no rate.
    let channels_only = resealed(&file, |forged| forged[65] = 2);
    assert_eq!(
        sprw::decode(&channels_only),
        Err(IoStatus::SoundNotDeclared)
    );
    // And the streaming reader agrees, which is what keeps it from being a
    // second, weaker door.
    assert_eq!(
        sprw::Spool::open(&rate_only.as_slice()),
        Err(IoStatus::SoundNotDeclared)
    );
}

#[test]
fn a_reel_with_no_sound_that_declares_samples_is_refused() {
    // Nought channels and ten thousand samples: a header claiming a length for
    // sound it says it does not have. Nothing in the payload contradicts it —
    // the file is exactly as long as its pictures — so only the header's own
    // consistency catches it.
    let file = sprw::encode(&sample()).expect("an encoding");
    let lying = resealed(&file, |forged| {
        forged[72..80].copy_from_slice(&10_000_u64.to_le_bytes());
    });
    assert_eq!(sprw::decode(&lying), Err(IoStatus::SoundNotDeclared));
    assert_eq!(
        sprw::Spool::open(&lying.as_slice()),
        Err(IoStatus::SoundNotDeclared)
    );
}

#[test]
fn a_reserved_byte_of_the_sound_description_is_refused() {
    // The six bytes between the channel count and the sample count. A reader
    // that ignored them would read a future version's field as nothing.
    let file = sprw::encode(&sample()).expect("an encoding");
    for index in 66..72 {
        let set = resealed(&file, |forged| forged[index] = 1);
        assert_eq!(
            sprw::decode(&set),
            Err(IoStatus::ReservedFieldSet),
            "byte {index} was ignored"
        );
    }
}
