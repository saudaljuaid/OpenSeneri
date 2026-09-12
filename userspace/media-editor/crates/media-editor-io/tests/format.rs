// SPDX-License-Identifier: GPL-3.0-only
//! The project file: what it accepts, and everything it refuses.
//!
//! The sweeps at the end are this crate's fuzzing until `cargo-fuzz` can be
//! brought through the dependency gate (R-11.3). They are deterministic, they
//! run in CI, and between them they put every byte of a real file, every
//! prefix of it, and a few hundred thousand bytes of garbage through the
//! decoder. The decoder may refuse; it may not misbehave.

use media_editor_core::{Digest, Duration, Instant, Rational, Timebase};
use media_editor_io::{FORMAT_VERSION, HEADER_BYTES, IoStatus, MAGIC, decode, encode};
use media_editor_model::curve::{Curve, Interpolation, Keyframe};
use media_editor_model::{Clip, Edit, Item, MediaAsset, Project, TrackKind};

const RATE: Timebase = Timebase::NTSC_30;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a non-negative length")
}

/// A project with two assets, two sequences, and a mixture of items.
fn sample() -> Project {
    let mut project = Project::new();
    let first = project
        .add_media(MediaAsset::new(Digest::of(b"first"), RATE, frames(17_982)).expect("an asset"))
        .expect("room");
    let second = project
        .add_media(MediaAsset::new(Digest::of(b"second"), RATE, frames(3_000)).expect("an asset"))
        .expect("room");

    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 1,
                kind: TrackKind::Audio,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(first, 300, frames(1_798)).expect("a clip")),
            },
        )
        .expect("a clip");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 1,
                item: Item::gap(frames(48)).expect("a gap"),
            },
        )
        .expect("a gap");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 2,
                item: Item::Clip(Clip::new(second, 0, frames(900)).expect("a clip")),
            },
        )
        .expect("a clip");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 1,
                index: 0,
                item: Item::Clip(Clip::new(first, 0, frames(2_746)).expect("a clip")),
            },
        )
        .expect("sound");

    look_at(&mut project, sequence);

    frame_up(&mut project, sequence);

    animate(&mut project, sequence);

    // Two notes, so the sweeps cover the count, the instant, the length and
    // the text -- and so a reader that wrote the same answer for every marker
    // would be caught. One of them empty, because an empty note is legal and
    // is the case a length-prefixed field gets wrong.
    for (tick, text) in [(0_i64, ""), (742, "the sync drifts here")] {
        project
            .apply(
                sequence,
                Edit::AddMarker {
                    at: Instant::new(tick, RATE),
                    text: std::string::String::from(text),
                },
            )
            .expect("a marker");
    }

    // A second, empty sequence, because one of everything is not a test.
    project.add_sequence(Timebase::PAL_25).expect("room");
    project
}

/// Grade one clip, and animate the grade's arrival on it.
///
/// Separate from the fixture for the reason `animate` is: `sample` had grown
/// past what one function should hold, and clippy says so rather than leaving
/// it to taste. Both halves belong here — the grade and the curve that brings
/// it on — because the curve is refused without the grade, so a fixture that
/// set one and not the other would not be a project the model can hold.
fn look_at(project: &mut Project, sequence: media_editor_model::SequenceId) {
    // A grade on one clip and not the others, so the sweeps cover both the
    // flag byte and the thirty-two that follow it, and so a reader that wrote
    // the same answer for every clip would be caught.
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(Digest::of(b"a look")),
            },
        )
        .expect("a grade");

    // And a curve bringing that grade on, on the same clip and not the others.
    // The fixture gains a value for a field in the commit the field arrives
    // in, which is the narrower form of a lesson this file has learned twice:
    // a sweep over bytes the writer never writes covers nothing, and every
    // round-trip test agrees perfectly about a field that is not there.
    //
    // Three keyframes with an ease among them, for the reason `animate` gives:
    // an ease is the only interpolation that writes anything after its tag.
    project
        .apply(
            sequence,
            Edit::SetClipGradeStrength {
                track: 0,
                index: 0,
                strength: Some(
                    Curve::new(std::vec![
                        Keyframe::new(
                            Instant::new(0, RATE),
                            Rational::new(0, 1).expect("a value"),
                            Interpolation::ease_in_out().expect("an ease"),
                        )
                        .expect("a keyframe"),
                        Keyframe::new(
                            Instant::new(240, RATE),
                            Rational::new(3, 5).expect("a value"),
                            Interpolation::Linear,
                        )
                        .expect("a keyframe"),
                        Keyframe::new(
                            Instant::new(1_200, RATE),
                            Rational::new(1, 1).expect("a value"),
                            Interpolation::Hold,
                        )
                        .expect("a keyframe"),
                    ])
                    .expect("a curve"),
                ),
            },
        )
        .expect("a strength");
}

/// Frame one clip and animate all four lanes of its framing.
///
/// The sweeps below run over `sample`, and until this existed **no motion was
/// in the file they sweep at all** — the motion tests build their own project,
/// so every byte of every lane was covered by a round trip and by nothing
/// else. That is the gap this file's own note about fixtures describes, sitting
/// in the file since M8.10 and found by asking where the turn's new lane would
/// be swept.
///
/// All four lanes, with an ease among the keyframes, because an ease is the
/// only interpolation that writes anything after its tag — and a format bug in
/// a variable-length record reads the next field as part of this one.
fn frame_up(project: &mut Project, sequence: media_editor_model::SequenceId) {
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 2,
                transform: Some(
                    media_editor_model::Transform::scaled(
                        Rational::new(3, 2).expect("a rational"),
                        Rational::new(3, 2).expect("a rational"),
                        (
                            Rational::new(1, 20).expect("a rational"),
                            Rational::new(-1, 16).expect("a rational"),
                        ),
                        media_editor_model::Resampling::Bilinear,
                    )
                    .expect("a transform")
                    // A pivot that is **not** the centre, so the sweeps cover
                    // bytes that differ from the default. A fixture carrying
                    // the default would sweep two rationals that any reader
                    // could guess, which is the same nothing as not sweeping
                    // them.
                    .with_anchor((
                        Rational::new(1, 5).expect("a rational"),
                        Rational::new(7, 8).expect("a rational"),
                    )),
                ),
            },
        )
        .expect("a framing");
    let lane = |from: (i64, i64), to: (i64, i64)| {
        Some(
            Curve::new(std::vec![
                Keyframe::new(
                    Instant::new(0, RATE),
                    Rational::new(from.0, from.1).expect("a value"),
                    Interpolation::ease_in_out().expect("an ease"),
                )
                .expect("a keyframe"),
                Keyframe::new(
                    Instant::new(600, RATE),
                    Rational::new(to.0, to.1).expect("a value"),
                    Interpolation::Linear,
                )
                .expect("a keyframe"),
            ])
            .expect("a curve"),
        )
    };
    project
        .apply(
            sequence,
            Edit::SetClipMotion {
                track: 0,
                index: 2,
                motion: Some(
                    media_editor_model::Motion::new(
                        lane((1, 1), (9, 4)),
                        lane((0, 1), (-3, 20)),
                        lane((1, 17), (5, 17)),
                        lane((0, 1), (2, 5)),
                    )
                    .expect("a motion"),
                ),
            },
        )
        .expect("a motion");
}

/// Put a curve on each automation lane.
///
/// Separate from the fixture only because the fixture had grown past what one
/// function should hold. Both lanes are animated, and that is the point: the
/// sweeps below cover bytes that are actually written, so a lane left empty
/// here is a lane the format could forget entirely without a test noticing.
/// It did — the first version of this animated only the picture track, and
/// dropping the level curve from the writer broke nothing at all.
fn animate(project: &mut Project, sequence: media_editor_model::SequenceId) {
    // An ease rather than a linear on at least one keyframe, because it is the
    // only interpolation that writes anything after its tag — and a format bug
    // in a variable-length record is the kind that reads the next field as
    // part of this one.
    project
        .apply(
            sequence,
            Edit::SetTrackOpacity {
                track: 0,
                opacity: Some(
                    Curve::new(std::vec![
                        Keyframe::new(
                            Instant::new(0, RATE),
                            Rational::new(0, 1).expect("a value"),
                            Interpolation::ease_in_out().expect("an ease"),
                        )
                        .expect("a keyframe"),
                        Keyframe::new(
                            Instant::new(300, RATE),
                            Rational::new(7, 8).expect("a value"),
                            Interpolation::Linear,
                        )
                        .expect("a keyframe"),
                        Keyframe::new(
                            Instant::new(1_798, RATE),
                            Rational::new(1, 3).expect("a value"),
                            Interpolation::Hold,
                        )
                        .expect("a keyframe"),
                    ])
                    .expect("a curve"),
                ),
            },
        )
        .expect("an automation");

    // And a fader curve on the sound track.
    project
        .apply(
            sequence,
            Edit::SetTrackLevel {
                track: 1,
                level: Some(
                    Curve::new(std::vec![
                        Keyframe::new(
                            Instant::new(0, RATE),
                            Rational::new(-60, 1).expect("a value"),
                            Interpolation::Linear,
                        )
                        .expect("a keyframe"),
                        Keyframe::new(
                            Instant::new(120, RATE),
                            Rational::new(-15, 2).expect("a value"),
                            Interpolation::ease_in_out().expect("an ease"),
                        )
                        .expect("a keyframe"),
                        Keyframe::new(
                            Instant::new(2_400, RATE),
                            Rational::new(0, 1).expect("a value"),
                            Interpolation::Hold,
                        )
                        .expect("a keyframe"),
                    ])
                    .expect("a curve"),
                ),
            },
        )
        .expect("an automation");
}

/// A project that has been through a file, which is what a loaded one is.
fn round_tripped(project: &Project) -> Project {
    decode(&encode(project).expect("an encoding")).expect("a decoding")
}

#[test]
fn a_project_survives_a_round_trip() {
    let original = round_tripped(&sample());
    let again = round_tripped(&original);
    assert_eq!(original, again, "the model is the same after a second trip");
}

#[test]
fn the_encoding_is_canonical() {
    let first = encode(&sample()).expect("an encoding");
    let reloaded = decode(&first).expect("a decoding");
    let second = encode(&reloaded).expect("an encoding");
    assert_eq!(
        first, second,
        "encoding a decoded file must reproduce it byte for byte"
    );
}

#[test]
fn a_curve_survives_the_file_with_every_keyframe_intact() {
    // Round-trip equality would pass for a format that wrote nothing and
    // rebuilt a default, so this looks at the curve itself: the instants, the
    // values as the exact fractions they are, and the interpolation of each
    // keyframe including an ease's four handles.
    let original = sample();
    let loaded = round_tripped(&original);
    let before = original
        .sequences()
        .iter()
        .next()
        .expect("a sequence")
        .1
        .track(0)
        .expect("a track")
        .opacity()
        .expect("an automation")
        .clone();
    let after = loaded
        .sequences()
        .iter()
        .next()
        .expect("a sequence")
        .1
        .track(0)
        .expect("a track")
        .opacity()
        .expect("an automation")
        .clone();
    assert_eq!(before, after);
    assert_eq!(after.keyframes().len(), 3);
    assert_eq!(
        after.keyframes()[2].value(),
        Rational::new(1, 3).expect("a value"),
        "a third is not a decimal and must not have become one"
    );
    assert!(matches!(
        after.keyframes()[0].interpolation(),
        Interpolation::Ease { .. }
    ));

    // And the curve still answers the same, which is what it is for.
    for frame in [0, 100, 300, 1_000, 1_798, 5_000] {
        assert_eq!(
            after.value_at(Instant::new(frame, RATE)),
            before.value_at(Instant::new(frame, RATE)),
            "frame {frame} reads differently after a trip through a file"
        );
    }
}

#[test]
fn a_grade_survives_the_file_and_only_the_clip_that_has_one() {
    // Named separately from the round trip because a reader that gave every
    // clip the same grade, or none, would round-trip perfectly against itself.
    let loaded = round_tripped(&sample());
    let track = loaded
        .sequences()
        .iter()
        .next()
        .expect("a sequence")
        .1
        .track(0)
        .expect("a track");

    let media_editor_model::Item::Clip(first) = track.item(0).expect("an item") else {
        panic!("not a clip");
    };
    assert_eq!(
        first.grade(),
        Some(Digest::of(b"a look")),
        "the grade did not survive"
    );

    // The gap at index one has none, and the second clip at index two was
    // never given one — so a writer that wrote a grade unconditionally, or a
    // reader that carried the last one forward, is caught here.
    let media_editor_model::Item::Clip(second) = track.item(2).expect("an item") else {
        panic!("not a clip");
    };
    assert_eq!(
        second.grade(),
        None,
        "a grade appeared on a clip without one"
    );
}

#[test]
fn a_byte_naming_neither_a_grade_nor_its_absence_is_refused() {
    // The flag is nought or one. Anything else is a file this reader does not
    // understand, and reading an unknown value as "no grade" would drop a look
    // while the file still said there was one.
    let file = encode(&sample()).expect("an encoding");
    let mut found = false;
    for index in HEADER_BYTES..file.len() {
        let mut mutated = file.clone();
        mutated[index] = 99;
        if matches!(decode(&mend(&mutated)), Err(IoStatus::UnknownGradeTag(99))) {
            found = true;
            break;
        }
    }
    assert!(found, "no byte in the file is a grade flag");
}

#[test]
fn both_automation_lanes_survive_the_file() {
    // Named separately from the curve round trip because they are separate
    // fields written one behind the other, and a reader that read the same one
    // twice, or read them in the wrong order, would round-trip a project whose
    // picture faded like its sound.
    let original = sample();
    let loaded = round_tripped(&original);
    let sequence = loaded.sequences().iter().next().expect("a sequence").1;

    let opacity = sequence
        .track(0)
        .expect("a track")
        .opacity()
        .expect("an opacity curve");
    let level = sequence
        .track(1)
        .expect("a track")
        .level()
        .expect("a level curve");
    assert_ne!(
        opacity.keyframes()[0].value(),
        level.keyframes()[0].value(),
        "the two lanes came back holding the same thing"
    );
    assert_eq!(
        level.keyframes()[0].value(),
        Rational::new(-60, 1).expect("a value")
    );
    assert_eq!(
        level.keyframes()[1].value(),
        Rational::new(-15, 2).expect("a value"),
        "minus seven and a half decibels is not a whole number and must not \
         have become one"
    );

    // The lanes do not cross: a picture track has no level and a sound track
    // has no opacity, and the model refuses either, so a reader that swapped
    // them would not have got this far.
    assert!(sequence.track(0).expect("a track").level().is_none());
    assert!(sequence.track(1).expect("a track").opacity().is_none());
}

#[test]
fn a_byte_naming_no_interpolation_is_refused() {
    // A tag this build does not know must be refused rather than defaulted:
    // reading an unknown interpolation as a hold would turn somebody's ease
    // into a step, silently, and the file would still say it was an ease.
    let file = encode(&sample()).expect("an encoding");
    let mut found = false;
    for index in HEADER_BYTES..file.len() {
        let mut mutated = file.clone();
        mutated[index] = 99;
        if matches!(
            decode(&mend(&mutated)),
            Err(IoStatus::UnknownInterpolationTag(99))
        ) {
            found = true;
            break;
        }
    }
    assert!(found, "no byte in the file is an interpolation tag");
}

/// Recompute a file's digest, so a deliberate edit reaches the checks past it.
///
/// This format's digest covers the payload alone, unlike the reel's and the
/// summary's, which cover their headers too. That is not an oversight here and
/// the byte sweep is the evidence: every field in this header — the magic, the
/// version, the reserved bytes, the payload length — is checked against
/// something else, so a mutation to any of them is refused on its own terms.
/// A summary's header carries the digest of the sound it summarises, which
/// nothing else can check, and that is why that one is inside its digest.
fn mend(file: &[u8]) -> std::vec::Vec<u8> {
    let mut held = file.to_vec();
    let digest = media_editor_core::Digest::of(&held[HEADER_BYTES..]);
    held[16..HEADER_BYTES].copy_from_slice(digest.bytes());
    held
}

#[test]
fn the_header_is_what_it_says_it_is() {
    let file = encode(&sample()).expect("an encoding");
    assert!(file.len() > HEADER_BYTES);
    assert_eq!(&file[..4], &MAGIC);
    assert_eq!(u16::from_le_bytes([file[4], file[5]]), FORMAT_VERSION);
    assert_eq!(u16::from_le_bytes([file[6], file[7]]), 0, "reserved");

    let mut length = [0_u8; 8];
    length.copy_from_slice(&file[8..16]);
    let declared = usize::try_from(u64::from_le_bytes(length)).expect("a length that fits");
    assert_eq!(declared, file.len() - HEADER_BYTES);

    let mut digest = [0_u8; 32];
    digest.copy_from_slice(&file[16..48]);
    assert_eq!(
        Digest::new(digest),
        Digest::of(&file[HEADER_BYTES..]),
        "the header's digest is the payload's"
    );
}

#[test]
fn history_is_not_saved() {
    let mut project = sample();
    assert!(project.history().undo_depth() > 0, "the sample was edited");
    project = round_tripped(&project);
    assert_eq!(
        project.history().undo_depth(),
        0,
        "a freshly opened project has nothing to undo"
    );
    assert_eq!(project.history().redo_depth(), 0);
}

#[test]
fn a_file_that_is_not_one_is_refused() {
    assert_eq!(decode(b""), Err(IoStatus::TruncatedHeader));
    assert_eq!(decode(&[0; 47]), Err(IoStatus::TruncatedHeader));

    let mut file = encode(&sample()).expect("an encoding");
    file[0] = b'X';
    assert_eq!(decode(&file), Err(IoStatus::NotAProjectFile));
}

#[test]
fn a_future_version_is_refused_by_number() {
    let mut file = encode(&sample()).expect("an encoding");
    file[4..6].copy_from_slice(&(FORMAT_VERSION + 1).to_le_bytes());
    assert_eq!(
        decode(&file),
        Err(IoStatus::UnsupportedVersion(FORMAT_VERSION + 1)),
        "a reader that guessed at a later format would be guessing at the user's work"
    );
}

#[test]
fn a_set_reserved_field_is_refused() {
    let mut file = encode(&sample()).expect("an encoding");
    file[6] = 1;
    assert_eq!(decode(&file), Err(IoStatus::ReservedFieldSet));
}

#[test]
fn a_wrong_length_is_refused_either_way() {
    let file = encode(&sample()).expect("an encoding");

    let mut short = file.clone();
    let declared = (file.len() - HEADER_BYTES) as u64;
    short[8..16].copy_from_slice(&(declared + 1).to_le_bytes());
    assert_eq!(decode(&short), Err(IoStatus::TruncatedPayload));

    let mut long = file.clone();
    long[8..16].copy_from_slice(&(declared - 1).to_le_bytes());
    assert_eq!(decode(&long), Err(IoStatus::TrailingBytes));
}

#[test]
fn a_damaged_payload_is_refused_rather_than_loaded() {
    let mut file = encode(&sample()).expect("an encoding");
    let middle = HEADER_BYTES + (file.len() - HEADER_BYTES) / 2;
    file[middle] ^= 0x01;
    assert_eq!(
        decode(&file),
        Err(IoStatus::DigestMismatch),
        "one flipped bit anywhere in a saved project is one flipped bit too many"
    );
}

#[test]
fn every_single_byte_change_is_refused() {
    // The strongest claim this format makes: there is no byte of a valid file
    // that can be changed to anything else and still be read. The digest
    // covers the payload, the header covers the digest, and the magic and
    // version cover the header.
    let file = encode(&sample()).expect("an encoding");
    let mut checked = 0_usize;
    for index in 0..file.len() {
        for replacement in [0x00_u8, 0x01, 0x7F, 0x80, 0xFF] {
            if file[index] == replacement {
                continue;
            }
            let mut mutated = file.clone();
            mutated[index] = replacement;
            assert!(
                decode(&mutated).is_err(),
                "byte {index} changed to {replacement:#04x} was accepted"
            );
            checked += 1;
        }
    }
    assert!(checked > 1000, "the sweep covered only {checked} mutations");
}

#[test]
fn every_prefix_of_a_file_is_refused() {
    let file = encode(&sample()).expect("an encoding");
    for length in 0..file.len() {
        assert!(
            decode(&file[..length]).is_err(),
            "a file truncated to {length} bytes was accepted"
        );
    }
    assert!(decode(&file).is_ok(), "the whole file is still fine");
}

#[test]
fn every_extension_of_a_file_is_refused() {
    let file = encode(&sample()).expect("an encoding");
    for extra in 1..64_usize {
        let mut extended = file.clone();
        extended.resize(file.len() + extra, 0);
        assert_eq!(decode(&extended), Err(IoStatus::TrailingBytes));
    }
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
    // A project file arrives from outside and is hostile until proven
    // otherwise (R-11). The decoder may refuse it; it may not loop, exhaust
    // memory, or panic.
    let mut generator = Generator(0x0BAD_C0DE_D15E_A5E1);
    for _ in 0..2000 {
        let length = usize::try_from(generator.next() % 512).unwrap_or(0);
        let mut bytes: std::vec::Vec<u8> = std::vec::Vec::with_capacity(length);
        for _ in 0..length {
            bytes.push(u8::try_from(generator.next() & 0xFF).unwrap_or(0));
        }
        assert!(decode(&bytes).is_err(), "random bytes were accepted");
    }
}

#[test]
fn garbage_behind_a_valid_header_is_refused() {
    // The harder case: the magic, version, length, and digest all agree, and
    // the payload is nonsense. Now every refusal has to come from the
    // structure itself.
    let mut generator = Generator(0x5EED_1234_5678_9ABC);
    let mut refusals = 0_usize;
    for _ in 0..4000 {
        let length = usize::try_from(generator.next() % 256).unwrap_or(0);
        let mut payload: std::vec::Vec<u8> = std::vec::Vec::with_capacity(length);
        for _ in 0..length {
            payload.push(u8::try_from(generator.next() & 0xFF).unwrap_or(0));
        }
        let mut file: std::vec::Vec<u8> = std::vec::Vec::new();
        file.extend_from_slice(&MAGIC);
        file.extend_from_slice(&FORMAT_VERSION.to_le_bytes());
        file.extend_from_slice(&0_u16.to_le_bytes());
        file.extend_from_slice(&(payload.len() as u64).to_le_bytes());
        file.extend_from_slice(Digest::of(&payload).bytes());
        file.extend_from_slice(&payload);

        // A well-formed empty payload is a real, empty project; anything else
        // must be refused by the structure.
        match decode(&file) {
            Ok(project) => assert!(
                project.media().is_empty() && project.sequences().is_empty(),
                "a nonsense payload decoded into a non-empty project"
            ),
            Err(_) => refusals += 1,
        }
    }
    assert!(
        refusals > 3000,
        "only {refusals} of 4000 payloads were refused"
    );
}

#[test]
fn a_clip_naming_media_the_file_does_not_have_is_refused() {
    // Hand-built: one sequence, no media, one track, one clip pointing at
    // media index zero. The sequence's *header* comes first and its body last,
    // which is the ordering nested sequences forced -- a media table has to be
    // able to name a sequence, and a sequence's body has to be able to name a
    // media asset.
    let mut payload: std::vec::Vec<u8> = std::vec::Vec::new();
    payload.extend_from_slice(&1_u32.to_le_bytes()); // one sequence
    payload.extend_from_slice(&30_000_i64.to_le_bytes()); // its timebase
    payload.extend_from_slice(&1_001_i64.to_le_bytes());
    payload.extend_from_slice(&0_u32.to_le_bytes()); // no media
    payload.extend_from_slice(&0_u32.to_le_bytes()); // this body is sequence nought
    payload.extend_from_slice(&1_u32.to_le_bytes()); // one track
    payload.push(0); // video
    payload.push(1); // a fader at a level
    payload.extend_from_slice(&0_i64.to_le_bytes()); // zero decibels
    payload.extend_from_slice(&1_i64.to_le_bytes());
    payload.extend_from_slice(&1_u32.to_le_bytes()); // one item
    payload.push(0); // a clip
    payload.extend_from_slice(&0_u32.to_le_bytes()); // media index zero
    payload.extend_from_slice(&0_i64.to_le_bytes());
    payload.extend_from_slice(&100_i64.to_le_bytes());

    let mut file: std::vec::Vec<u8> = std::vec::Vec::new();
    file.extend_from_slice(&MAGIC);
    file.extend_from_slice(&FORMAT_VERSION.to_le_bytes());
    file.extend_from_slice(&0_u16.to_le_bytes());
    file.extend_from_slice(&(payload.len() as u64).to_le_bytes());
    file.extend_from_slice(Digest::of(&payload).bytes());
    file.extend_from_slice(&payload);

    assert_eq!(decode(&file), Err(IoStatus::MediaIndexOutOfRange));
}

#[test]
fn a_faders_position_survives_the_file() {
    // A mix that did not survive a save would be a mix nobody could deliver.
    // Every kind of position is checked, including the two that are easy to
    // conflate: a track at the bottom of its travel, and a track that is off.
    use media_editor_core::Rational;
    use media_editor_model::{Fader, MINIMUM_DECIBELS};

    let positions = [
        Fader::UNITY,
        Fader::MUTED,
        Fader::at(Rational::new(MINIMUM_DECIBELS, 1).expect("a ratio")).expect("a level"),
        Fader::at(Rational::new(-15, 2).expect("a ratio")).expect("a level"),
        Fader::at(Rational::new(24, 1).expect("a ratio")).expect("a level"),
    ];

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    for (index, fader) in positions.iter().enumerate() {
        project
            .apply(
                sequence,
                Edit::AddTrack {
                    index,
                    kind: TrackKind::Audio,
                },
            )
            .expect("a track");
        project
            .apply(
                sequence,
                Edit::SetTrackFader {
                    track: index,
                    fader: *fader,
                },
            )
            .expect("a fader move");
    }

    let file = encode(&project).expect("an encoding");
    let back = decode(&file).expect("a decoding");
    for (index, fader) in positions.iter().enumerate() {
        assert_eq!(
            back.sequence(sequence)
                .expect("a sequence")
                .track(index)
                .expect("a track")
                .fader(),
            *fader,
            "track {index}"
        );
    }

    // A quarter of a decibel is not a round number in any binary, which is why
    // the fader is stored as the exact ratio the user set rather than as a
    // factor: it comes back as the same ratio, not as one near it.
    assert_eq!(
        back.sequence(sequence)
            .expect("a sequence")
            .track(3)
            .expect("a track")
            .fader()
            .decibels(),
        Some(Rational::new(-15, 2).expect("a ratio"))
    );
}

#[test]
fn a_version_one_project_is_refused_rather_than_read_as_version_two() {
    // Version one had no fader, so a version-one file read as version two
    // would take its faders out of whatever followed the track kind — which is
    // an item count. The version number is what stops that, and it is checked
    // before any field is.
    let project = sample();
    let mut file = encode(&project).expect("an encoding");
    file[4] = 1;
    file[5] = 0;
    assert_eq!(decode(&file), Err(IoStatus::UnsupportedVersion(1)));
}

#[test]
fn a_fader_tag_this_build_does_not_know_is_refused() {
    use media_editor_core::Rational;
    use media_editor_model::Fader;

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Audio,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::SetTrackFader {
                track: 0,
                fader: Fader::at(Rational::new(-6, 1).expect("a ratio")).expect("a level"),
            },
        )
        .expect("a fader move");

    let file = encode(&project).expect("an encoding");
    // The digest covers the payload, so a mutated file is caught by that
    // first. This finds the tag byte by rebuilding the file around a bad one
    // instead, so it is the tag reader that refuses rather than the digest.
    let mut forged = file.clone();
    // The fader tag is the byte 1 that precedes the exact ratio -6/1, which
    // nothing else in this file can be.
    let needle = {
        let mut bytes = std::vec::Vec::new();
        bytes.push(1_u8);
        bytes.extend_from_slice(&(-6_i64).to_le_bytes());
        bytes.extend_from_slice(&1_i64.to_le_bytes());
        bytes
    };
    let at = forged
        .windows(needle.len())
        .position(|window| window == needle.as_slice())
        .expect("the fader is in the file");
    forged[at] = 9;
    let payload = forged[HEADER_BYTES..].to_vec();
    let digest = Digest::of(&payload);
    forged[HEADER_BYTES - 32..HEADER_BYTES].copy_from_slice(digest.bytes());

    assert_eq!(decode(&forged), Err(IoStatus::UnknownFaderTag(9)));
}

#[test]
fn a_dissolve_survives_the_file() {
    use media_editor_model::Transition;

    let mut project = Project::new();
    let asset = MediaAsset::new(Digest::of(b"a"), RATE, frames(10_000)).expect("an asset");
    let id = project.add_media(asset).expect("an identifier");
    let sequence = project.add_sequence(RATE).expect("a sequence");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    for (index, source_start) in [(0, 0_i64), (1, 100), (2, 200)] {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index,
                    item: Item::Clip(Clip::new(id, source_start, frames(50)).expect("a clip")),
                },
            )
            .expect("an insert");
    }
    // Two dissolves, on the two cuts, of different lengths — so a reader that
    // wrote one length for both, or lost the order, would be caught.
    let transitions = [
        Transition::new(1, frames(12)).expect("a dissolve"),
        Transition::new(2, frames(30)).expect("a dissolve"),
    ];
    for transition in transitions {
        project
            .apply(
                sequence,
                Edit::AddTransition {
                    track: 0,
                    transition,
                },
            )
            .expect("a dissolve");
    }

    let back = round_tripped(&project);
    let track = back
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track");
    assert_eq!(track.transitions(), &transitions);
}

#[test]
fn a_file_whose_dissolve_the_model_would_refuse_is_refused() {
    use media_editor_model::Transition;

    // A well-formed file, then the same file with its dissolve moved onto a
    // cut that has a gap on one side. The digest is recomputed so that it is
    // the model's refusal doing the work rather than the integrity check.
    let mut project = Project::new();
    let asset = MediaAsset::new(Digest::of(b"a"), RATE, frames(10_000)).expect("an asset");
    let id = project.add_media(asset).expect("an identifier");
    let sequence = project.add_sequence(RATE).expect("a sequence");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    for (index, item) in [
        Item::Clip(Clip::new(id, 0, frames(50)).expect("a clip")),
        Item::Clip(Clip::new(id, 100, frames(50)).expect("a clip")),
        Item::gap(frames(50)).expect("a gap"),
    ]
    .into_iter()
    .enumerate()
    {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index,
                    item,
                },
            )
            .expect("an insert");
    }
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(12)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");

    let file = encode(&project).expect("an encoding");
    assert!(decode(&file).is_ok());

    // The boundary is written as a u32 immediately before the duration, so
    // finding the pair identifies it without guessing at offsets.
    let needle = {
        let mut bytes = std::vec::Vec::new();
        bytes.extend_from_slice(&1_u32.to_le_bytes());
        bytes.extend_from_slice(&12_i64.to_le_bytes());
        bytes
    };
    let mut forged = file.clone();
    let at = forged
        .windows(needle.len())
        .position(|window| window == needle.as_slice())
        .expect("the dissolve is in the file");
    forged[at..at + 4].copy_from_slice(&2_u32.to_le_bytes());
    let payload = forged[HEADER_BYTES..].to_vec();
    forged[HEADER_BYTES - 32..HEADER_BYTES].copy_from_slice(Digest::of(&payload).bytes());

    assert_eq!(
        decode(&forged),
        Err(IoStatus::Model(media_editor_model::ModelStatus::NotAClip)),
        "a dissolve onto a gap is refused on the way in, not stored and \
         discovered later"
    );
}

#[test]
fn a_wipe_survives_the_file_with_its_direction() {
    // Version seven's whole reason. A transition used to be a boundary and a
    // length, and reading one back could only produce a dissolve because that
    // was the only kind there was. The tag says which, and the direction is
    // written as two rationals rather than an angle, so a wipe drawn at a
    // third of a turn is the same wipe when it comes back rather than a
    // rounding of one.
    use media_editor_model::{Transition, TransitionKind, Wipe};

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"wiped"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    for index in 0..2 {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index,
                    item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
                },
            )
            .expect("a clip");
    }
    let wipe = Wipe::new(
        Rational::new(3, 7).expect("a rational"),
        Rational::new(-2, 5).expect("a rational"),
    )
    .expect("a wipe");
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::wiping(1, frames(12), wipe).expect("a wipe"),
            },
        )
        .expect("a wipe");

    let back = round_tripped(&project);
    let held = back
        .sequence(back.sequences().iter().next().expect("a sequence").0)
        .expect("a sequence");
    let transition = held.track(0).expect("a track").transitions()[0];
    assert_eq!(transition.kind(), TransitionKind::Wipe(wipe));
    assert_eq!(
        back.sequence(back.sequences().iter().next().expect("a sequence").0)
            .expect("a sequence"),
        project.sequence(sequence).expect("a sequence"),
        "and the whole sequence comes back equal, not just the direction"
    );
}

#[test]
fn a_transition_tag_this_build_does_not_read_is_refused() {
    use media_editor_model::Transition;

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"tagged"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    for index in 0..2 {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index,
                    item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
                },
            )
            .expect("a clip");
    }
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(12)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");

    // The tag check cannot be reached by mutating a byte: the digest refuses
    // first, which is exactly what the byte sweep in this file asserts. So the
    // file is *resealed* around the changed byte -- the payload's digest
    // recomputed and written back into the header -- which is what a reader
    // has to survive being handed by something that meant it.
    let file = encode(&project).expect("an encoding");
    let mut found = None;
    for index in HEADER_BYTES..file.len() {
        if file[index] != 0 {
            continue;
        }
        let mut broken = file.clone();
        broken[index] = 2;
        let sealed = Digest::of(&broken[HEADER_BYTES..]);
        broken[16..48].copy_from_slice(sealed.bytes());
        if decode(&broken) == Err(IoStatus::UnknownTransitionTag(2)) {
            found = Some(index);
            break;
        }
    }
    assert!(
        found.is_some(),
        "the transition kind has to be a tag some byte carries, or it is not a tag"
    );
}

#[test]
fn a_wipes_softness_survives_the_file() {
    use media_editor_model::{Transition, TransitionKind, Wipe};

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"soft"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    for index in 0..2 {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index,
                    item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
                },
            )
            .expect("a clip");
    }
    // A third is the value that catches a format storing softness as anything
    // binary: it is exact as a rational and is not exact as any fixed-point
    // number this project could have reached for.
    let wipe = Wipe::soft(
        Rational::ONE,
        Rational::ZERO,
        Rational::new(1, 3).expect("a third"),
    )
    .expect("a wipe");
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::wiping(1, frames(12), wipe).expect("a wipe"),
            },
        )
        .expect("a wipe");

    let back = round_tripped(&project);
    let held = back
        .sequence(back.sequences().iter().next().expect("a sequence").0)
        .expect("a sequence");
    let TransitionKind::Wipe(read) = held.track(0).expect("a track").transitions()[0].kind() else {
        panic!("a wipe went in and something else came out");
    };
    assert_eq!(read.softness(), Rational::new(1, 3).expect("a third"));
    assert_eq!(read, wipe);
}

#[test]
fn a_mask_survives_the_file_with_its_corners_and_its_inversion() {
    use media_editor_model::Mask;

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"masked"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    // Sevenths and elevenths: exact as rationals, and not exact as anything
    // binary. A mask stored as fixed-point would come back a different shape.
    let mask = Mask::new(vec![
        (
            Rational::new(1, 7).expect("a rational"),
            Rational::new(2, 11).expect("a rational"),
        ),
        (
            Rational::new(6, 7).expect("a rational"),
            Rational::new(3, 11).expect("a rational"),
        ),
        (
            Rational::new(5, 7).expect("a rational"),
            Rational::new(9, 11).expect("a rational"),
        ),
    ])
    .expect("a triangle")
    .inverted();
    project
        .apply(
            sequence,
            Edit::SetClipMask {
                track: 0,
                index: 0,
                mask: Some(mask.clone()),
            },
        )
        .expect("a mask");

    let back = round_tripped(&project);
    assert_eq!(
        back.sequence(back.sequences().iter().next().expect("a sequence").0)
            .expect("a sequence"),
        project.sequence(sequence).expect("a sequence"),
        "the whole sequence comes back equal, corners and inversion included"
    );
}

#[test]
fn a_file_holding_a_concave_mask_is_refused() {
    // The model refuses a concave outline, and the file goes through the same
    // constructor -- so a project cannot hold a shape by being written down
    // that it could not hold by being edited.
    //
    // Reaching that check needs a *resealed* file, because the digest refuses
    // a mutated byte long before any field is parsed. And it needs the right
    // bytes: the corner is written as two rationals, each a numerator and a
    // denominator, so the third corner of an eight-by-eight square is the
    // eight consecutive little-endian words `8, 1, 8, 1`. Pulling that corner
    // in to (3, 3) turns the square into an arrowhead.
    use media_editor_model::Mask;

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"bent"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    let mask = Mask::rectangle(
        Rational::ZERO,
        Rational::ZERO,
        Rational::from_integer(8),
        Rational::from_integer(8),
    )
    .expect("a square");
    project
        .apply(
            sequence,
            Edit::SetClipMask {
                track: 0,
                index: 0,
                mask: Some(mask),
            },
        )
        .expect("a mask");

    let file = encode(&project).expect("an encoding");
    let mut wanted = std::vec::Vec::new();
    for value in [8_i64, 1, 8, 1] {
        wanted.extend_from_slice(&value.to_le_bytes());
    }
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the third corner is in the file");

    let mut bent = file.clone();
    for offset in [0_usize, 16] {
        bent[at + offset..at + offset + 8].copy_from_slice(&3_i64.to_le_bytes());
    }
    let sealed = Digest::of(&bent[HEADER_BYTES..]);
    bent[16..48].copy_from_slice(sealed.bytes());

    assert_eq!(
        decode(&bent),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::MaskNotConvex
        )),
        "a file describing a shape the model would refuse has to be refused too"
    );
}

#[test]
fn a_location_hint_survives_the_file_uninterpreted() {
    use media_editor_model::Location;

    let mut project = Project::new();
    let id = project
        .add_media(MediaAsset::new(Digest::of(b"placed"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    // Not valid text, on purpose: a path is whatever the platform says it is,
    // and a format that round-trips only the paths it can read is a format
    // that loses somebody's media.
    let hint = Location::new(&[0xFF, b'/', 0x00, 0x80, b'r']).expect("a hint");
    project
        .set_media_location(id, Some(hint.clone()))
        .expect("an asset");

    let back = round_tripped(&project);
    let (_, asset) = back.media().iter().next().expect("an asset");
    assert_eq!(asset.location(), Some(&hint));
}

#[test]
fn an_asset_with_no_hint_costs_four_bytes_and_comes_back_with_none() {
    let mut project = Project::new();
    project
        .add_media(MediaAsset::new(Digest::of(b"bare"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let back = round_tripped(&project);
    let (_, asset) = back.media().iter().next().expect("an asset");
    assert_eq!(asset.location(), None, "absent is absent, not empty");
}

#[test]
fn a_file_listing_one_piece_of_content_twice_is_refused() {
    // `add_media` hands back the identifier it already had, which is right for
    // the API and wrong for a file: every clip indexing the second record
    // would then point at the first, which is a different programme arrived at
    // silently. Reaching the check needs a resealed file, because the digest
    // refuses a mutated byte first.
    let mut project = Project::new();
    for tag in [b"one".as_slice(), b"two".as_slice()] {
        project
            .add_media(MediaAsset::new(Digest::of(tag), RATE, frames(9_000)).expect("an asset"))
            .expect("room");
    }
    let file = encode(&project).expect("an encoding");

    // The media table follows the sequence headers, and this project has no
    // sequences -- so the count of nought is four bytes, the media count is
    // four more, and the first record's digest begins after both.
    let first: Vec<u8> = file[HEADER_BYTES + 8..HEADER_BYTES + 40].to_vec();
    let second = file
        .windows(32)
        .position(|window| window == Digest::of(b"two".as_slice()).bytes().as_slice())
        .expect("the second digest is in the file");
    let mut doubled = file.clone();
    doubled[second..second + 32].copy_from_slice(&first);
    let sealed = Digest::of(&doubled[HEADER_BYTES..]);
    doubled[16..48].copy_from_slice(sealed.bytes());

    assert_eq!(decode(&doubled), Err(IoStatus::DuplicateMedia));
}

#[test]
fn a_transform_survives_the_file_with_its_filter() {
    use media_editor_model::{Resampling, Transform};

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"moved"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    // A rotation nobody could write as a decimal: the linear part is four
    // rationals, so a third of the way is a third and not a rounding of one.
    let transform = Transform::new(
        [
            Rational::new(1, 3).expect("a rational"),
            Rational::new(-2, 7).expect("a rational"),
            Rational::new(2, 7).expect("a rational"),
            Rational::new(1, 3).expect("a rational"),
        ],
        (
            Rational::new(1, 11).expect("a rational"),
            Rational::new(-3, 13).expect("a rational"),
        ),
        Resampling::Bilinear,
    )
    .expect("a transform");
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: Some(transform),
            },
        )
        .expect("a transform");

    let back = round_tripped(&project);
    assert_eq!(
        back.sequence(back.sequences().iter().next().expect("a sequence").0)
            .expect("a sequence"),
        project.sequence(sequence).expect("a sequence"),
        "the whole sequence comes back equal, filter included"
    );
}

#[test]
fn a_file_holding_a_transform_that_flattens_the_picture_is_refused() {
    // The reader goes through the model's constructor, so a project cannot
    // hold a transform by being written down that it could not hold by being
    // edited. Reaching that check needs a resealed file.
    use media_editor_model::{Resampling, Transform};

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"flat"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: Some(
                    Transform::scaled(
                        Rational::from_integer(4),
                        Rational::from_integer(4),
                        (Rational::ZERO, Rational::ZERO),
                        Resampling::Area,
                    )
                    .expect("a transform"),
                ),
            },
        )
        .expect("a transform");

    // The linear part is `4, 0, 0, 4`. Making the second entry a four as well
    // gives `4, 4, 0, 4`, which is still invertible -- so instead zero the
    // *last* one, leaving `4, 0, 0, 0`, whose determinant is nought.
    let file = encode(&project).expect("an encoding");
    let mut wanted = std::vec::Vec::new();
    for value in [4_i64, 1, 0, 1, 0, 1, 4, 1] {
        wanted.extend_from_slice(&value.to_le_bytes());
    }
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the linear part is in the file");
    let mut flattened = file.clone();
    flattened[at + 48..at + 56].copy_from_slice(&0_i64.to_le_bytes());
    let sealed = Digest::of(&flattened[HEADER_BYTES..]);
    flattened[16..48].copy_from_slice(sealed.bytes());

    assert_eq!(
        decode(&flattened),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::TransformNotInvertible
        ))
    );
}

/// A project with one clip, framed at four times size, and animated.
///
/// The scale of four is deliberate: its linear part encodes as the eight
/// integers `4, 1, 0, 1, 0, 1, 4, 1`, which is a distinctive enough run of
/// bytes to find in a file and cut about.
fn animated(motion: media_editor_model::Motion) -> Project {
    use media_editor_model::{Resampling, Transform};

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"animated"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: Some(
                    Transform::scaled(
                        Rational::from_integer(4),
                        Rational::from_integer(4),
                        (Rational::ZERO, Rational::ZERO),
                        Resampling::Area,
                    )
                    .expect("a transform"),
                ),
            },
        )
        .expect("a framing");
    project
        .apply(
            sequence,
            Edit::SetClipMotion {
                track: 0,
                index: 0,
                motion: Some(motion),
            },
        )
        .expect("a motion");
    project
}

/// Where the animated fixture's linear part begins in its encoding.
fn linear_part(file: &[u8]) -> usize {
    let mut wanted = std::vec::Vec::new();
    for value in [4_i64, 1, 0, 1, 0, 1, 4, 1] {
        wanted.extend_from_slice(&value.to_le_bytes());
    }
    file.windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the linear part is in the file")
}

/// The file with its declared length and its digest recomputed.
///
/// Both, because a file is sealed twice over: the header states how long the
/// payload is and then states its digest, and a mutation that changes the
/// length is refused as truncated before anything gets far enough to check
/// the digest. A test that resealed only the digest would be testing the
/// length check.
fn resealed(mut file: std::vec::Vec<u8>) -> std::vec::Vec<u8> {
    let length = (file.len() - HEADER_BYTES) as u64;
    file[8..16].copy_from_slice(&length.to_le_bytes());
    let sealed = Digest::of(&file[HEADER_BYTES..]);
    file[16..48].copy_from_slice(sealed.bytes());
    file
}

/// A curve from `from` to `to` over forty-eight ticks, leaving on an ease.
fn animation(from: Rational, to: Rational) -> Curve {
    Curve::new(std::vec![
        Keyframe::new(
            Instant::new(0, RATE),
            from,
            Interpolation::ease_in_out().expect("an ease")
        )
        .expect("a keyframe"),
        Keyframe::new(Instant::new(48, RATE), to, Interpolation::Linear).expect("a keyframe"),
    ])
    .expect("a curve")
}

#[test]
fn a_motion_survives_the_file_with_all_four_lanes() {
    let motion = media_editor_model::Motion::new(
        Some(animation(
            Rational::ONE,
            Rational::new(7, 3).expect("a rational"),
        )),
        Some(animation(
            Rational::ZERO,
            Rational::new(-1, 11).expect("a rational"),
        )),
        Some(animation(
            Rational::new(1, 13).expect("a rational"),
            Rational::new(2, 13).expect("a rational"),
        )),
        // The turn's lane gains a value in the commit the lane arrives in,
        // which is the operational form of a lesson this file has learned
        // twice: a sweep over bytes the writer never writes covers nothing.
        Some(animation(
            Rational::new(-1, 3).expect("a rational"),
            Rational::new(5, 7).expect("a rational"),
        )),
    )
    .expect("a motion");
    let project = animated(motion);
    let back = round_tripped(&project);
    assert_eq!(
        back.sequence(back.sequences().iter().next().expect("a sequence").0)
            .expect("a sequence"),
        project
            .sequence(project.sequences().iter().next().expect("a sequence").0)
            .expect("a sequence"),
        "four lanes, their handles, and their sevenths all come back equal"
    );
}

#[test]
fn a_motions_turn_lane_survives_the_file_by_name() {
    // Named rather than round-tripped, because a round trip cannot see a field
    // the format has forgotten -- both sides would be missing it and would
    // agree. And the *values* rather than only the keyframes, so a lane that
    // came back with the right shape at the wrong instants would fail.
    let project = animated(
        media_editor_model::Motion::new(
            None,
            None,
            None,
            Some(animation(
                Rational::ZERO,
                Rational::new(1, 2).expect("a rational"),
            )),
        )
        .expect("a motion"),
    );
    let back = round_tripped(&project);
    let sequence = back.sequences().iter().next().expect("a sequence").1;
    let Item::Clip(clip) = sequence
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item")
    else {
        panic!("a clip");
    };
    let motion = clip.motion().expect("an animation");
    assert!(
        motion.scale().is_none() && motion.across().is_none() && motion.down().is_none(),
        "a lane nobody set came back set"
    );
    let lane = motion.turn().expect("the turn's lane did not come back");
    assert_eq!(
        lane.value_at(Instant::new(0, RATE)).expect("a value"),
        Rational::ZERO,
        "the turn lane's first keyframe is not where it was written"
    );
    // And the parameter really is a parameter: a half is the three-four-five
    // turn, derived by hand from `cos = (1 - t^2)/(1 + t^2)`.
    let turn = media_editor_model::Turn::from_half_angle(
        lane.value_at(Instant::new(1_798, RATE)).expect("a value"),
    )
    .expect("a turn");
    assert_eq!(
        (turn.cosine(), turn.sine()),
        (
            Rational::new(3, 5).expect("a rational"),
            Rational::new(4, 5).expect("a rational")
        ),
        "the lane came back holding something other than the half-angle it was          written with"
    );
}

#[test]
fn a_motion_lane_that_is_absent_stays_absent() {
    // Not the same as a lane holding its neutral. A clip that animates only
    // its scale must come back animating only its scale, or every save would
    // quietly grow two curves nobody drew -- and the next editor to look would
    // find keyframes on a move they never touched.
    let project = animated(
        media_editor_model::Motion::new(
            Some(animation(
                Rational::ONE,
                Rational::new(3, 2).expect("a rational"),
            )),
            None,
            None,
            None,
        )
        .expect("a motion"),
    );
    let back = round_tripped(&project);
    let sequence = back.sequences().iter().next().expect("a sequence").0;
    let Item::Clip(clip) = back
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item")
    else {
        panic!("a clip");
    };
    let motion = clip.motion().expect("an animation");
    assert!(motion.scale().is_some());
    assert!(motion.across().is_none());
    assert!(motion.down().is_none());
}

#[test]
fn a_file_holding_an_animation_with_nothing_to_animate_is_refused() {
    // The invariant the edit enforces, asked of the decoder. Reaching it needs
    // the transform cut out of a clip that keeps its motion: the flag goes to
    // nought and its hundred and twenty-nine bytes of payload go with it -- a
    // resampling byte, sixty-four of linear part, thirty-two of move and
    // thirty-two of pivot -- so everything after still lines up and the file
    // is resealed over what is left.
    let project = animated(
        media_editor_model::Motion::new(
            Some(animation(
                Rational::ONE,
                Rational::new(3, 2).expect("a rational"),
            )),
            None,
            None,
            None,
        )
        .expect("a motion"),
    );
    let file = encode(&project).expect("an encoding");
    let at = linear_part(&file);
    let mut stripped = file[..at - 1].to_vec();
    stripped.extend_from_slice(&file[at + 128..]);
    stripped[at - 2] = 0;

    assert_eq!(
        decode(&resealed(stripped)),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::NoTransformToAnimate
        ))
    );
}

#[test]
fn a_motion_tag_this_build_does_not_read_is_refused() {
    let project = animated(
        media_editor_model::Motion::new(
            Some(animation(
                Rational::ONE,
                Rational::new(3, 2).expect("a rational"),
            )),
            None,
            None,
            None,
        )
        .expect("a motion"),
    );
    let file = encode(&project).expect("an encoding");
    let at = linear_part(&file);
    let mut unknown = file.clone();
    // Straight after the linear part's sixty-four bytes, the offset's
    // thirty-two and the pivot's thirty-two: the byte that says whether an
    // animation follows.
    assert_eq!(unknown[at + 128], 1, "the fixture's clip is animated");
    unknown[at + 128] = 2;

    assert_eq!(
        decode(&resealed(unknown)),
        Err(IoStatus::UnknownMotionTag(2))
    );
}

#[test]
fn a_file_holding_a_scale_that_flattens_the_picture_is_refused() {
    // Through the model's own constructor, like every other field: a scale
    // keyframe of nought is refused when it is set, and a file that carries
    // one is refused when it is read.
    let project = animated(
        media_editor_model::Motion::new(
            Some(animation(
                Rational::from_integer(3),
                Rational::new(3, 2).expect("a rational"),
            )),
            None,
            None,
            None,
        )
        .expect("a motion"),
    );
    let file = encode(&project).expect("an encoding");
    // The first keyframe's value: a three over a one, written after its tick.
    let mut wanted = std::vec::Vec::new();
    for value in [0_i64, 3, 1] {
        wanted.extend_from_slice(&value.to_le_bytes());
    }
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the first keyframe is in the file");
    let mut flattened = file.clone();
    flattened[at + 8..at + 16].copy_from_slice(&0_i64.to_le_bytes());

    assert_eq!(
        decode(&resealed(flattened)),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::ScaleNotPositive
        ))
    );
}

/// A project holding one title card, and nothing else.
fn carded(text: &str) -> Project {
    use media_editor_model::Title;

    let mut project = Project::new();
    let title = Title::line(
        text.into(),
        Rational::new(1, 6).expect("a size"),
        Rational::new(3, 7).expect("a place"),
        Rational::new(5, 11).expect("a place"),
    )
    .expect("a title");
    let media = project
        .add_media(MediaAsset::titled(title, RATE, frames(240)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 0, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    project
}

#[test]
fn a_title_survives_the_file_with_its_words_and_its_place() {
    let project = carded("MEDIAEDTO");
    let back = round_tripped(&project);
    let (id, asset) = back.media().iter().next().expect("an asset");
    let title = asset.title().expect("a title");
    assert_eq!(title.lines(), ["MEDIAEDTO"]);
    assert_eq!(title.size(), Rational::new(1, 6).expect("a size"));
    assert_eq!(title.across(), Rational::new(3, 7).expect("a place"));
    assert_eq!(title.down(), Rational::new(5, 11).expect("a place"));
    assert_eq!(
        back.media().get(id).expect("an asset").digest(),
        project.media().iter().next().expect("an asset").1.digest(),
        "and it comes back named the same thing"
    );
}

#[test]
fn a_recording_and_a_title_are_told_apart_in_the_file() {
    // The tag costs one byte on every asset, which is the price of the format
    // never having to guess. Both kinds in one file, so a reader that got the
    // branch wrong would produce a project with the wrong number of titles in
    // it rather than one that merely failed.
    let mut project = carded("CARD");
    let recorded = project
        .add_media(MediaAsset::new(Digest::of(b"footage"), RATE, frames(240)).expect("an asset"))
        .expect("room");
    let hinted = media_editor_model::Location::new(b"/reels/a.sprw").expect("a hint");
    project
        .set_media_location(recorded, Some(hinted))
        .expect("a hint");

    let back = round_tripped(&project);
    let titles = back
        .media()
        .iter()
        .filter(|(_, asset)| asset.title().is_some())
        .count();
    let recordings = back
        .media()
        .iter()
        .filter(|(_, asset)| asset.title().is_none())
        .count();
    assert_eq!((titles, recordings), (1, 1));
    assert!(
        back.media()
            .iter()
            .any(|(_, asset)| asset.location().is_some()),
        "and the recording kept its hint"
    );
}

#[test]
fn a_title_whose_name_is_not_what_it_says_is_refused() {
    // A title is *named by* its description, so the two are one fact written
    // twice. A file where they disagree has been edited, and recomputing the
    // digest and accepting it would silently repoint every clip of the card.
    let project = carded("MEDIAEDTO");
    let file = encode(&project).expect("an encoding");
    let at = file
        .windows(9)
        .position(|window| window == b"MEDIAEDTO")
        .expect("the words are in the file");
    let mut edited = file.clone();
    edited[at + 3] = b'X';

    assert_eq!(
        decode(&resealed(edited)),
        Err(IoStatus::TitleDigestMismatch)
    );
}

#[test]
fn a_media_source_tag_this_build_does_not_read_is_refused() {
    let project = carded("MEDIAEDTO");
    let file = encode(&project).expect("an encoding");
    let at = file
        .windows(9)
        .position(|window| window == b"MEDIAEDTO")
        .expect("the words are in the file");
    // Straight before the words: the line's length, before that the count of
    // lines, and before that the source tag.
    let mut unknown = file.clone();
    assert_eq!(unknown[at - 9], 1, "the fixture's asset is a title");
    unknown[at - 9] = 7;

    assert_eq!(
        decode(&resealed(unknown)),
        Err(IoStatus::UnknownMediaSourceTag(7))
    );
}

#[test]
fn a_title_whose_words_are_not_text_is_refused() {
    let project = carded("MEDIAEDTO");
    let file = encode(&project).expect("an encoding");
    let at = file
        .windows(9)
        .position(|window| window == b"MEDIAEDTO")
        .expect("the words are in the file");
    let mut mangled = file.clone();
    // A byte no UTF-8 sequence starts or continues with.
    mangled[at + 2] = 0xFF;

    assert_eq!(decode(&resealed(mangled)), Err(IoStatus::TitleNotText));
}

#[test]
fn a_title_that_a_file_says_is_somewhere_is_refused() {
    // The invariant the model enforces, enforced again in the decoder -- and
    // the decoder's half needs a file no sequence of edits could write, so it
    // is built here: a titled asset's own bytes with a location hint spliced
    // into the empty slot in front of its source tag.
    //
    // Without that splice this test would be asserting the model's refusal a
    // second time and the decoder's line would have no test at all, which is
    // the shape a guard is usually missing in.
    let project = carded("MEDIAEDTO");
    let file = encode(&project).expect("an encoding");
    let at = file
        .windows(9)
        .position(|window| window == b"MEDIAEDTO")
        .expect("the words are in the file");
    // The source tag sits nine bytes before the words -- one tag, a four-byte
    // count of lines and a four-byte length -- and the location's own length
    // is the four before that.
    let tag = at - 9;
    let length = tag - 4;
    assert_eq!(&file[length..tag], &0_u32.to_le_bytes(), "no hint today");

    let hint = b"/nowhere/at/all";
    let mut hinted = file[..length].to_vec();
    hinted.extend_from_slice(&u32::try_from(hint.len()).expect("a length").to_le_bytes());
    hinted.extend_from_slice(hint);
    hinted.extend_from_slice(&file[tag..]);

    assert_eq!(
        decode(&resealed(hinted)),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::NotRecordedMedia
        ))
    );
}

#[test]
fn a_card_of_several_lines_survives_the_file_with_its_alignment() {
    use media_editor_model::{Alignment, Title};

    let mut project = Project::new();
    let title = Title::new(
        std::vec!["Media Editor".into(), String::new(), "MMXXVI".into()],
        Rational::new(1, 8).expect("a size"),
        Rational::new(2, 5).expect("a place"),
        Rational::new(3, 8).expect("a place"),
        Alignment::Right,
    )
    .expect("a title");
    let named = title.digest().expect("a digest");
    project
        .add_media(MediaAsset::titled(title, RATE, frames(240)).expect("an asset"))
        .expect("room");

    let back = round_tripped(&project);
    let (_, asset) = back.media().iter().next().expect("an asset");
    let card = asset.title().expect("a title");
    assert_eq!(card.lines(), ["Media Editor", "", "MMXXVI"]);
    assert_eq!(card.alignment(), Alignment::Right);
    assert_eq!(
        asset.digest(),
        named,
        "and it comes back named the same thing, which is only true if every \
         line and the alignment came back"
    );
}

#[test]
fn an_alignment_tag_this_build_does_not_read_is_refused() {
    use media_editor_model::{Alignment, Title};

    let mut project = Project::new();
    let title = Title::new(
        std::vec!["CARD".into()],
        Rational::new(1, 8).expect("a size"),
        Rational::new(1, 2).expect("a place"),
        Rational::new(1, 2).expect("a place"),
        Alignment::Centre,
    )
    .expect("a title");
    project
        .add_media(MediaAsset::titled(title, RATE, frames(240)).expect("an asset"))
        .expect("room");
    let file = encode(&project).expect("an encoding");
    let at = file
        .windows(4)
        .position(|window| window == b"CARD")
        .expect("the words are in the file");
    // Straight after the only line: the alignment.
    let mut unknown = file.clone();
    assert_eq!(unknown[at + 4], 1, "the fixture's card is centred");
    unknown[at + 4] = 9;

    assert_eq!(
        decode(&resealed(unknown)),
        Err(IoStatus::UnknownAlignmentTag(9))
    );
}

#[test]
fn a_clips_fades_survive_the_file() {
    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"faded"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    project
        .apply(
            sequence,
            Edit::SetClipFades {
                track: 0,
                index: 0,
                fade_in: frames(7),
                fade_out: frames(11),
            },
        )
        .expect("fades");

    let back = round_tripped(&project);
    assert_eq!(
        back.sequence(back.sequences().iter().next().expect("a sequence").0)
            .expect("a sequence"),
        project.sequence(sequence).expect("a sequence"),
        "the whole sequence comes back equal, both fades included"
    );
    // And they are not the same number, so a reader that swapped them would be
    // caught rather than being right by accident.
    assert_ne!(frames(7), frames(11));
}

#[test]
fn a_fade_tag_this_build_does_not_read_is_refused() {
    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"faded"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    project
        .apply(
            sequence,
            Edit::SetClipFades {
                track: 0,
                index: 0,
                fade_in: frames(7),
                fade_out: frames(11),
            },
        )
        .expect("fades");
    let file = encode(&project).expect("an encoding");
    // The two fade lengths, and the tag straight before them.
    let mut wanted = std::vec::Vec::new();
    for value in [7_i64, 11] {
        wanted.extend_from_slice(&value.to_le_bytes());
    }
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the fades are in the file");
    let mut unknown = file.clone();
    assert_eq!(unknown[at - 1], 1, "the fixture's clip is faded");
    unknown[at - 1] = 4;

    assert_eq!(decode(&resealed(unknown)), Err(IoStatus::UnknownFadeTag(4)));
}

#[test]
fn a_file_whose_fades_outlast_its_clip_is_refused() {
    // Through the model's own constructor, like everything else: a file cannot
    // hold a clip the model would have refused. Reaching it needs a resealed
    // file, because no sequence of edits can write one.
    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"faded"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    project
        .apply(
            sequence,
            Edit::SetClipFades {
                track: 0,
                index: 0,
                fade_in: frames(7),
                fade_out: frames(11),
            },
        )
        .expect("fades");
    let file = encode(&project).expect("an encoding");
    let mut wanted = std::vec::Vec::new();
    for value in [7_i64, 11] {
        wanted.extend_from_slice(&value.to_le_bytes());
    }
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the fades are in the file");
    let mut greedy = file.clone();
    greedy[at..at + 8].copy_from_slice(&40_i64.to_le_bytes());

    assert_eq!(
        decode(&resealed(greedy)),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::FadesLongerThanClip
        ))
    );
}

/// A project with one clip at a given speed.
fn retimed(speed: Rational) -> Project {
    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"retimed"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    project
        .apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: media_editor_model::Playback::At(speed),
            },
        )
        .expect("a speed");
    project
}

#[test]
fn a_speed_survives_the_file_exactly() {
    // Twenty-four twenty-fifths, which no decimal writes: the whole reason a
    // speed is a rational is that this comes back as itself rather than as a
    // rounding that drifts a frame every twenty-five seconds.
    let speed = Rational::new(24, 25).expect("a pull-down");
    let project = retimed(speed);
    let back = round_tripped(&project);
    assert_eq!(
        back.sequence(back.sequences().iter().next().expect("a sequence").0)
            .expect("a sequence"),
        project
            .sequence(project.sequences().iter().next().expect("a sequence").0)
            .expect("a sequence")
    );
    let sequence = back.sequences().iter().next().expect("a sequence").0;
    let Item::Clip(clip) = back
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item")
    else {
        panic!("a clip");
    };
    assert_eq!(clip.speed(), Some(speed));
}

#[test]
fn a_reversed_clip_survives_the_file() {
    let project = retimed(Rational::new(-1, 2).expect("reverse"));
    let back = round_tripped(&project);
    let sequence = back.sequences().iter().next().expect("a sequence").0;
    let Item::Clip(clip) = back
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item")
    else {
        panic!("a clip");
    };
    assert_eq!(clip.speed(), Some(Rational::new(-1, 2).expect("reverse")));
}

#[test]
fn a_clip_at_real_time_costs_one_byte_to_say_so() {
    // A flag rather than a fraction of one over one, which is sixteen bytes of
    // saying nothing -- and real time is what almost every clip is.
    let real = encode(&retimed(Rational::ONE)).expect("an encoding");
    let slow = encode(&retimed(Rational::new(1, 2).expect("a half"))).expect("an encoding");
    assert_eq!(slow.len(), real.len() + 16, "the fraction is the only cost");
}

#[test]
fn a_speed_tag_this_build_does_not_read_is_refused() {
    let project = retimed(Rational::new(1, 2).expect("a half"));
    let file = encode(&project).expect("an encoding");
    let mut wanted = std::vec::Vec::new();
    for value in [1_i64, 2] {
        wanted.extend_from_slice(&value.to_le_bytes());
    }
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the speed is in the file");
    let mut unknown = file.clone();
    assert_eq!(unknown[at - 1], 1, "the fixture's clip is retimed");
    unknown[at - 1] = 6;

    assert_eq!(
        decode(&resealed(unknown)),
        Err(IoStatus::UnknownSpeedTag(6))
    );
}

#[test]
fn a_file_holding_a_clip_stopped_at_no_speed_is_refused() {
    // Through the model's own constructor, like everything else. The name of
    // this test used to say "frozen", which stopped being right the day a
    // freeze became a tag of its own: a still is `SPEED_FROZEN` and carries no
    // number, and a *speed* of nought is a number that means nothing. Only the
    // second is what this refuses.
    let project = retimed(Rational::new(1, 2).expect("a half"));
    let file = encode(&project).expect("an encoding");
    let mut wanted = std::vec::Vec::new();
    for value in [1_i64, 2] {
        wanted.extend_from_slice(&value.to_le_bytes());
    }
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the speed is in the file");
    let mut stopped = file.clone();
    stopped[at..at + 8].copy_from_slice(&0_i64.to_le_bytes());

    assert_eq!(
        decode(&resealed(stopped)),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::SpeedNotUsable
        ))
    );
}

/// A project holding one coloured title card.
fn inked(ink: media_editor_model::Ink) -> Project {
    use media_editor_model::Title;

    let mut project = Project::new();
    let title = Title::line(
        "MEDIAEDTO".into(),
        Rational::new(1, 6).expect("a size"),
        Rational::new(3, 7).expect("a place"),
        Rational::new(5, 11).expect("a place"),
    )
    .expect("a title")
    .with_ink(ink);
    project
        .add_media(MediaAsset::titled(title, RATE, frames(240)).expect("an asset"))
        .expect("room");
    project
}

#[test]
fn an_ink_survives_the_file_exactly() {
    use media_editor_model::Ink;

    // Exactly, and the fixture says so: three fractions no decimal can write.
    // An ink that went through a float on the way to the file would come back
    // near enough to pass a comparison of pictures and wrong in the digest,
    // which is the one place a title cannot afford to be near enough.
    let ink = Ink::new(
        Rational::new(1, 3).expect("a fraction"),
        Rational::new(7, 9).expect("a fraction"),
        Rational::new(2, 11).expect("a fraction"),
    )
    .expect("an ink");
    let project = inked(ink);
    let back = round_tripped(&project);
    let (_, asset) = back.media().iter().next().expect("an asset");
    assert_eq!(asset.title().expect("a title").ink(), ink);
    assert_eq!(
        asset.digest(),
        project.media().iter().next().expect("an asset").1.digest(),
        "and the card is still named the same thing, ink and all"
    );
}

#[test]
fn a_white_card_costs_one_byte_to_say_so() {
    use media_editor_model::Ink;

    // Three rationals is forty-eight bytes, and white is what most cards are.
    // Measured rather than asserted from the constant, because the constant is
    // what would be wrong.
    let white = encode(&inked(Ink::WHITE)).expect("an encoding").len();
    let coloured = encode(&inked(
        Ink::new(Rational::ONE, Rational::ZERO, Rational::ZERO).expect("red"),
    ))
    .expect("an encoding")
    .len();
    assert_eq!(coloured - white, 48, "three rationals after the tag");
}

#[test]
fn an_ink_tag_this_build_does_not_read_is_refused() {
    use media_editor_model::Ink;

    // Not read as white. A future build that adds a second way of naming a
    // colour -- a gradient, a per-line ink -- would write a tag this one has
    // never seen, and a decoder that fell through to white would hand back
    // somebody's card in the wrong colour and call it a successful load.
    let project = inked(Ink::WHITE);
    let file = encode(&project).expect("an encoding");
    let at = file
        .windows(9)
        .position(|window| window == b"MEDIAEDTO")
        .expect("the words are in the file");
    // After the words: the alignment, then three rationals, then the ink tag.
    let tag = at + 9 + 1 + 3 * 16;
    let mut unknown = file.clone();
    assert_eq!(unknown[tag], 0, "the fixture's card is white");
    unknown[tag] = 9;
    assert_eq!(decode(&resealed(unknown)), Err(IoStatus::UnknownInkTag(9)));
}

#[test]
fn a_file_holding_an_ink_the_model_refuses_is_refused() {
    use media_editor_model::Ink;

    // The decoder builds every ink through `Ink::new`, so a file cannot carry
    // a colour brighter than white -- which would reach the compositor as a
    // premultiplied sample above its own coverage and be refused there, three
    // layers further on and with nothing left to say which card it came from.
    let project = inked(Ink::new(Rational::ONE, Rational::ZERO, Rational::ZERO).expect("red"));
    let file = encode(&project).expect("an encoding");
    let at = file
        .windows(9)
        .position(|window| window == b"MEDIAEDTO")
        .expect("the words are in the file");
    // The first channel's numerator, straight after the ink tag.
    let numerator = at + 9 + 1 + 3 * 16 + 1;
    let mut brighter = file.clone();
    assert_eq!(
        i64::from_le_bytes(
            brighter[numerator..numerator + 8]
                .try_into()
                .expect("eight bytes")
        ),
        1,
        "the fixture's red channel is one over one"
    );
    brighter[numerator..numerator + 8].copy_from_slice(&2_i64.to_le_bytes());
    assert_eq!(
        decode(&resealed(brighter)),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::InkOutOfRange
        ))
    );
}

#[test]
fn a_frozen_clip_survives_the_file() {
    use media_editor_model::Playback;

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"still"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    project
        .apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::Frozen,
            },
        )
        .expect("a freeze");
    let back = round_tripped(&project);
    let held = back.sequences().iter().next().expect("a sequence").0;
    let Item::Clip(clip) = back
        .sequence(held)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item")
    else {
        panic!("a clip");
    };
    assert!(clip.is_frozen(), "a still comes back a still");
    assert_eq!(clip.speed(), None);
    assert_eq!(clip.source_start(), 200, "of the frame it was held on");
}

#[test]
fn a_freeze_costs_one_byte_and_no_number() {
    use media_editor_model::Playback;

    // The frame a freeze holds is the clip's in point, which is already in the
    // file a few bytes above the tag. Writing it again would be the same fact
    // twice, and two facts that have to be kept agreeing.
    let real = encode(&retimed(Rational::ONE)).expect("an encoding").len();
    let mut project = retimed(Rational::ONE);
    let sequence = project.sequences().iter().next().expect("a sequence").0;
    project
        .apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::Frozen,
            },
        )
        .expect("a freeze");
    assert_eq!(
        encode(&project).expect("an encoding").len(),
        real,
        "a tag either way, and nothing after the frozen one"
    );
}

#[test]
fn a_clips_own_opacity_curve_survives_the_file() {
    use media_editor_model::{Curve, Interpolation, Keyframe};

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"shot"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    // An ease, so the four handles go through the file as well as the values.
    let drawn = Curve::new(std::vec![
        Keyframe::new(
            media_editor_core::Instant::new(0, RATE),
            Rational::ZERO,
            Interpolation::ease_in_out().expect("an ease"),
        )
        .expect("a keyframe"),
        Keyframe::new(
            media_editor_core::Instant::new(24, RATE),
            Rational::ONE,
            Interpolation::Linear,
        )
        .expect("a keyframe"),
    ])
    .expect("a curve");
    project
        .apply(
            sequence,
            Edit::SetClipOpacity {
                track: 0,
                index: 0,
                opacity: Some(drawn.clone()),
            },
        )
        .expect("a rise");
    let back = round_tripped(&project);
    let held = back.sequences().iter().next().expect("a sequence").0;
    let Item::Clip(clip) = back
        .sequence(held)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item")
    else {
        panic!("a clip");
    };
    assert_eq!(clip.opacity(), Some(&drawn));
}

#[test]
fn a_clip_nobody_animated_costs_four_bytes_to_say_so() {
    use media_editor_model::{Curve, Interpolation, Keyframe};

    // A count of nought, which is what an absent lane has always cost -- the
    // same four bytes each of the motion's three lanes costs, and no new tag
    // to reserve. Measured rather than asserted from the constant.
    let plain = encode(&retimed(Rational::ONE)).expect("an encoding").len();
    let mut project = retimed(Rational::ONE);
    let sequence = project.sequences().iter().next().expect("a sequence").0;
    project
        .apply(
            sequence,
            Edit::SetClipOpacity {
                track: 0,
                index: 0,
                opacity: Some(
                    Curve::new(std::vec![
                        Keyframe::new(
                            media_editor_core::Instant::new(0, RATE),
                            Rational::ONE,
                            Interpolation::Hold,
                        )
                        .expect("a keyframe"),
                    ])
                    .expect("a curve"),
                ),
            },
        )
        .expect("an animation");
    let animated = encode(&project).expect("an encoding").len();
    assert_eq!(
        animated - plain,
        25,
        "one keyframe: a tick, a rational and a kind, past the count both pay"
    );
}

#[test]
fn a_clips_mask_animation_survives_the_file() {
    use media_editor_model::{Curve, Interpolation, Keyframe, Mask, Motion};

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"shot"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    let iris = Motion::new(
        Some(
            Curve::new(std::vec![
                Keyframe::new(
                    media_editor_core::Instant::new(0, RATE),
                    Rational::ONE,
                    Interpolation::ease_in_out().expect("an ease"),
                )
                .expect("a keyframe"),
                Keyframe::new(
                    media_editor_core::Instant::new(24, RATE),
                    Rational::new(2, 1).expect("twice"),
                    Interpolation::Linear,
                )
                .expect("a keyframe"),
            ])
            .expect("a curve"),
        ),
        None,
        None,
        None,
    )
    .expect("an iris");
    for edit in [
        Edit::SetClipMask {
            track: 0,
            index: 0,
            mask: Some(
                Mask::rectangle(
                    Rational::new(1, 4).expect("a quarter"),
                    Rational::new(1, 4).expect("a quarter"),
                    Rational::new(3, 4).expect("three quarters"),
                    Rational::new(3, 4).expect("three quarters"),
                )
                .expect("a square"),
            ),
        },
        Edit::SetClipMaskMotion {
            track: 0,
            index: 0,
            motion: Some(iris.clone()),
        },
    ] {
        project.apply(sequence, edit).expect("an edit");
    }
    let back = round_tripped(&project);
    let held = back.sequences().iter().next().expect("a sequence").0;
    let Item::Clip(clip) = back
        .sequence(held)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item")
    else {
        panic!("a clip");
    };
    assert_eq!(clip.mask_motion(), Some(&iris));
}

#[test]
fn a_file_animating_a_mask_that_is_not_there_is_refused() {
    use media_editor_model::{Curve, Interpolation, Keyframe, Mask, Motion};

    // The invariant the edit enforces, asked of the decoder -- reached the way
    // the transform's version is: cut the *shape* out of a clip that keeps its
    // animation. The mask's tag goes to nought and its payload goes with it,
    // so everything after still lines up, and the file is resealed over what
    // is left.
    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"shot"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    for edit in [
        Edit::SetClipMask {
            track: 0,
            index: 0,
            mask: Some(
                Mask::rectangle(
                    Rational::new(1, 4).expect("a quarter"),
                    Rational::new(1, 4).expect("a quarter"),
                    Rational::new(3, 4).expect("three quarters"),
                    Rational::new(3, 4).expect("three quarters"),
                )
                .expect("a square"),
            ),
        },
        Edit::SetClipMaskMotion {
            track: 0,
            index: 0,
            motion: Some(
                Motion::new(
                    Some(
                        Curve::new(std::vec![
                            Keyframe::new(
                                media_editor_core::Instant::new(0, RATE),
                                Rational::ONE,
                                Interpolation::Linear,
                            )
                            .expect("a keyframe"),
                        ])
                        .expect("a curve"),
                    ),
                    None,
                    None,
                    None,
                )
                .expect("an iris"),
            ),
        },
    ] {
        project.apply(sequence, edit).expect("an edit");
    }
    let file = encode(&project).expect("an encoding");

    // The first corner is (1/4, 1/4): four little-endian i64s, 1, 4, 1, 4.
    let mut wanted = std::vec::Vec::new();
    for value in [1_i64, 4, 1, 4] {
        wanted.extend_from_slice(&value.to_le_bytes());
    }
    let corners = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the shape is in the file");
    // Before the corners: a count of four, an inversion flag, and the tag.
    let tag = corners - 4 - 1 - 1;
    assert_eq!(file[tag], 1, "the fixture's clip really is masked");
    let mut stripped = file[..tag].to_vec();
    stripped.push(0);
    // Four corners of two rationals, sixteen bytes each.
    stripped.extend_from_slice(&file[corners + 4 * 2 * 16..]);

    assert_eq!(
        decode(&resealed(stripped)),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::NoMaskToAnimate
        ))
    );
}

/// A project with one graded clip, and a curve on it if asked.
fn graded(strength: Option<Curve>) -> (Project, std::vec::Vec<u8>) {
    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"graded"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(Digest::of(b"a look")),
            },
        )
        .expect("a grade");
    if let Some(curve) = strength {
        project
            .apply(
                sequence,
                Edit::SetClipGradeStrength {
                    track: 0,
                    index: 0,
                    strength: Some(curve),
                },
            )
            .expect("a strength");
    }
    let file = encode(&project).expect("an encoding");
    (project, file)
}

/// The curve those tests animate a grade with: nought, rising to all of it.
fn arrival() -> Curve {
    Curve::new(std::vec![
        Keyframe::new(
            Instant::new(0, RATE),
            Rational::new(0, 1).expect("a value"),
            Interpolation::Linear,
        )
        .expect("a keyframe"),
        Keyframe::new(
            Instant::new(24, RATE),
            Rational::new(1, 1).expect("a value"),
            Interpolation::Hold,
        )
        .expect("a keyframe"),
    ])
    .expect("a curve")
}

#[test]
fn a_clips_grade_strength_survives_the_file() {
    // Named rather than round-tripped, because a round trip cannot see a field
    // the format has forgotten: both sides of the comparison would be missing
    // it and would agree perfectly. This file's own notes record that
    // happening twice, once immediately after the lesson was written down.
    let (_, file) = graded(Some(arrival()));
    let back = decode(&file).expect("a project");
    let Item::Clip(clip) = back
        .sequences()
        .iter()
        .next()
        .expect("a sequence")
        .1
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item")
    else {
        panic!("a clip");
    };
    assert_eq!(
        clip.grade_strength(),
        Some(&arrival()),
        "the arrival did not come back"
    );
    // And the values it reads, not only the keyframes it holds — so a curve
    // that came back with the right shape at the wrong instants would fail.
    for (offset, expected) in [(0_i64, (0, 1)), (6, (1, 4)), (24, (1, 1)), (47, (1, 1))] {
        assert_eq!(
            clip.grade_strength_at(offset).expect("a strength"),
            Rational::new(expected.0, expected.1).expect("a value"),
            "the arrival reads wrongly at offset {offset}"
        );
    }
}

#[test]
fn a_grade_nobody_animated_costs_four_bytes_to_say_so() {
    // The count of nought an absent lane has always been written as, rather
    // than a fifth tag byte to reserve. Measured rather than reasoned about:
    // the difference between a file with the lane empty and one with a curve
    // on it is the curve, and the difference between this format version and
    // the last is four bytes a clip.
    let (_, flat) = graded(None);
    let (_, animated) = graded(Some(arrival()));
    assert_eq!(
        animated.len() - flat.len(),
        50,
        "two keyframes are eight bytes of instant, sixteen of value and one of \
         interpolation each, and the count of nought is already paid for"
    );
}

#[test]
fn a_file_that_brings_on_a_grade_that_is_not_there_is_refused() {
    // Through the model's own constructor, so a file cannot produce a project
    // no sequence of edits could. Reaching the check needs a *resealed* file:
    // the payload's digest covers every byte, so a plain mutation is refused
    // as a digest mismatch long before a field is looked at.
    let (_, animated) = graded(Some(arrival()));
    let (_, flat) = graded(None);
    // Find the grade's flag byte and the digest behind it, and cut both out —
    // which leaves the strength's curve exactly where it was, now describing a
    // clip with no look to be the strength of.
    let mut wanted = std::vec::Vec::new();
    wanted.push(1_u8);
    wanted.extend_from_slice(Digest::of(b"a look").bytes());
    let at = animated
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the grade is in the file");
    let mut ungraded = animated.clone();
    ungraded.splice(at..at + wanted.len(), [0_u8]);
    assert_eq!(
        ungraded.len(),
        animated.len() - 32,
        "the splice did not remove exactly the digest"
    );
    assert!(
        flat.len() < ungraded.len(),
        "and what is left still carries the curve, which is the whole point"
    );

    assert_eq!(
        decode(&resealed(ungraded)),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::NoGradeToAnimate
        )),
        "a file holding an arrival with nothing to arrive was accepted"
    );
}

#[test]
fn a_transforms_pivot_survives_the_file() {
    // Named rather than round-tripped, for the reason this file has now
    // learned three times: a round trip cannot see a field the format has
    // forgotten, because both sides are missing it and agree.
    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"framed"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 200, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    let pivot = (
        Rational::new(-1, 3).expect("a rational"),
        Rational::new(9, 7).expect("a rational"),
    );
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: Some(
                    media_editor_model::Transform::scaled(
                        Rational::new(2, 1).expect("a rational"),
                        Rational::new(2, 1).expect("a rational"),
                        (Rational::ZERO, Rational::ZERO),
                        media_editor_model::Resampling::Area,
                    )
                    .expect("a transform")
                    // Outside the frame on both axes, deliberately: a pivot is
                    // not required to be inside the picture, and a reader that
                    // clamped one would be caught here rather than by a shot
                    // that swings in from the wrong place.
                    .with_anchor(pivot),
                ),
            },
        )
        .expect("a framing");

    let back = round_tripped(&project);
    let sequence = back.sequences().iter().next().expect("a sequence").1;
    let Item::Clip(clip) = sequence
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item")
    else {
        panic!("a clip");
    };
    assert_eq!(
        clip.transform().expect("a framing").anchor(),
        pivot,
        "the pivot did not come back"
    );
    // And a transform nobody moved the pivot of comes back on the centre,
    // rather than on whatever nought happens to mean.
    assert_eq!(
        media_editor_model::Transform::scaled(
            Rational::ONE,
            Rational::ONE,
            (Rational::ZERO, Rational::ZERO),
            media_editor_model::Resampling::Area,
        )
        .expect("a transform")
        .anchor(),
        (
            Rational::new(1, 2).expect("a rational"),
            Rational::new(1, 2).expect("a rational")
        ),
    );
}

#[test]
fn a_sequences_markers_survive_the_file() {
    // Named rather than round-tripped, for the reason this file has learned
    // four times: a round trip cannot see a field the format has forgotten,
    // because both sides are missing it and agree.
    let back = round_tripped(&sample());
    let sequence = back.sequences().iter().next().expect("a sequence").1;
    let notes: std::vec::Vec<(i64, &str)> = sequence
        .markers()
        .iter()
        .map(|held| (held.at().ticks(), held.text()))
        .collect();
    assert_eq!(
        notes,
        std::vec![(0_i64, ""), (742, "the sync drifts here")],
        "the notes did not come back, in order, saying what they said"
    );
}

#[test]
fn a_sequence_with_no_markers_costs_four_bytes_to_say_so() {
    // The count of nought every list in this format is written as. Measured
    // rather than reasoned about, and measured *per sequence* -- which is what
    // says the step is not per file.
    let mut one = Project::new();
    one.add_sequence(RATE).expect("room");
    let mut two = Project::new();
    two.add_sequence(RATE).expect("room");
    two.add_sequence(RATE).expect("room");

    let single = encode(&one).expect("bytes").len();
    let double = encode(&two).expect("bytes").len();
    // A second empty sequence is its timebase in the header block, then its
    // own index, its track count and its marker count in the body: sixteen,
    // then four, four and four.
    assert_eq!(double - single, 28);
}

#[test]
fn a_markers_text_has_to_be_text() {
    // The same refusal a title's words carry, and reaching it needs a resealed
    // file: the payload's digest covers every byte, so a plain mutation is
    // refused as a digest mismatch long before a field is looked at.
    let file = encode(&sample()).expect("an encoding");
    let wanted = b"the sync drifts here";
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the note is in the file");
    let mut mangled = file.clone();
    // A byte no UTF-8 sequence may begin with.
    mangled[at] = 0xFF;
    assert_eq!(
        decode(&resealed(mangled)),
        Err(IoStatus::MarkerNotText),
        "a file's bytes became a note that says something else"
    );
}

#[test]
fn a_file_holding_two_markers_at_one_instant_is_refused() {
    // Through the model's own constructor, like everything else: a file cannot
    // produce a project no sequence of edits could. Reaching it needs a
    // resealed file, because no sequence of edits can write one.
    let file = encode(&sample()).expect("an encoding");
    // The second note's instant, 742, immediately before its length and text.
    let wanted = 742_i64.to_le_bytes();
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the note's instant is in the file");
    let mut collided = file.clone();
    // Move it onto the first note's instant, which is nought.
    collided[at..at + 8].copy_from_slice(&0_i64.to_le_bytes());
    assert_eq!(
        decode(&resealed(collided)),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::MarkerExists
        )),
    );
}

#[test]
fn a_file_holding_a_marker_before_the_programme_is_refused() {
    let file = encode(&sample()).expect("an encoding");
    let wanted = 742_i64.to_le_bytes();
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the note's instant is in the file");
    let mut early = file.clone();
    early[at..at + 8].copy_from_slice(&(-1_i64).to_le_bytes());
    assert_eq!(
        decode(&resealed(early)),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::MarkerBeforeStart
        )),
    );
}

/// A project of two sequences, the second nested inside the first.
fn nested() -> Project {
    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"footage"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    // The *inner* sequence is added second on purpose, so that the file's
    // sequence order and the nesting order are not the same order -- a fixture
    // where the nest happened to come first could not tell a reader that
    // resolved by position from one that resolved by luck.
    let outer = project.add_sequence(RATE).expect("room");
    let inner = project.add_sequence(RATE).expect("room");
    for (id, length) in [(outer, 200_i64), (inner, 60)] {
        project
            .apply(
                id,
                Edit::AddTrack {
                    index: 0,
                    kind: TrackKind::Video,
                },
            )
            .expect("a track");
        project
            .apply(
                id,
                Edit::InsertItem {
                    track: 0,
                    index: 0,
                    item: Item::Clip(Clip::new(media, 0, frames(length)).expect("a clip")),
                },
            )
            .expect("a clip");
    }
    let nest = project
        .add_media(MediaAsset::nesting(inner, RATE, frames(60)).expect("an asset"))
        .expect("room");
    project
        .apply(
            outer,
            Edit::InsertItem {
                track: 0,
                index: 1,
                item: Item::Clip(Clip::new(nest, 0, frames(40)).expect("a clip")),
            },
        )
        .expect("a nested clip");
    project
}

#[test]
fn a_nest_survives_the_file_naming_the_sequence_it_named() {
    // The whole reason the layout has three phases. A nest names a sequence by
    // position, and a clip names media by position, so each table refers to
    // the other -- and what breaks the knot is that only a sequence's timebase
    // is needed to create it.
    let back = round_tripped(&nested());
    let ids: std::vec::Vec<_> = back.sequences().iter().map(|(id, _)| id).collect();
    assert_eq!(ids.len(), 2);

    let (_, asset) = back
        .media()
        .iter()
        .find(|(_, held)| held.nested().is_some())
        .expect("a nest came back");
    assert_eq!(
        asset.nested(),
        Some(ids[1]),
        "the nest came back naming the wrong sequence"
    );
    assert_eq!(asset.duration(), frames(60));
    assert_eq!(asset.location(), None);
}

#[test]
fn a_nest_naming_a_sequence_the_file_does_not_hold_is_refused() {
    // The nest's sequence index, mutated past the end. Reaching it needs a
    // resealed file, because the payload's digest refuses a mutated byte long
    // before a field is looked at.
    let file = encode(&nested()).expect("an encoding");
    // The nested asset's tag is 2, followed by its sequence index of one.
    let mut wanted = std::vec::Vec::new();
    wanted.push(2_u8);
    wanted.extend_from_slice(&1_u32.to_le_bytes());
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the nest is in the file");
    let mut lost = file.clone();
    lost[at + 1..at + 5].copy_from_slice(&7_u32.to_le_bytes());
    assert_eq!(
        decode(&resealed(lost)),
        Err(IoStatus::SequenceIndexOutOfRange)
    );
}

#[test]
fn a_nest_whose_digest_is_not_its_sequences_is_refused() {
    // A nest is named by which sequence it is, and the reader recomputes that
    // rather than believing the file. Otherwise a file could give two nests
    // one digest, or give a nest a *recording's* digest, and the library would
    // fold them together.
    let file = encode(&nested()).expect("an encoding");
    let mut wanted = std::vec::Vec::new();
    wanted.push(2_u8);
    wanted.extend_from_slice(&1_u32.to_le_bytes());
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the nest is in the file");
    // The record's digest is the first thirty-two bytes of it, and the source
    // tag is the last byte before the payload above: back up over the tag, the
    // location count, the duration and the timebase.
    let record = at - 4 - 8 - 16 - 32;
    let mut mangled = file.clone();
    mangled[record] ^= 0xFF;
    assert_eq!(
        decode(&resealed(mangled)),
        Err(IoStatus::NestDigestMismatch)
    );
}

#[test]
fn a_nest_carrying_a_location_is_refused() {
    // A nest has nowhere to be, for the reason a title has nowhere to be. The
    // location's length is the four bytes before the source tag.
    let file = encode(&nested()).expect("an encoding");
    let mut wanted = std::vec::Vec::new();
    wanted.extend_from_slice(&0_u32.to_le_bytes());
    wanted.push(2_u8);
    wanted.extend_from_slice(&1_u32.to_le_bytes());
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the nest is in the file");
    let mut hinted = file.clone();
    // One byte of hint, spliced in where the reader will look for it.
    hinted.splice(at..at + 4, 1_u32.to_le_bytes());
    hinted.insert(at + 4, b'/');
    assert_eq!(decode(&resealed(hinted)), Err(IoStatus::NestHasLocation));
}

#[test]
fn a_file_whose_nesting_is_a_cycle_is_refused() {
    // Through the model's own checks, like everything else: a file cannot
    // produce a project no sequence of edits could. Built by pointing the
    // *outer* sequence's nest at the outer sequence itself, which is index
    // nought rather than one.
    let file = encode(&nested()).expect("an encoding");
    let mut wanted = std::vec::Vec::new();
    wanted.push(2_u8);
    wanted.extend_from_slice(&1_u32.to_le_bytes());
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the nest is in the file");
    let mut looped = file.clone();
    looped[at + 1..at + 5].copy_from_slice(&0_u32.to_le_bytes());
    // The digest no longer matches the sequence it names, which is the *first*
    // thing the reader checks -- so this proves the two refusals are ordered
    // rather than that a cycle is accepted. The cycle itself is refused by the
    // model, and `a_sequence_cannot_contain_itself` in the model's own suite
    // is where that is pinned.
    assert_eq!(decode(&resealed(looped)), Err(IoStatus::NestDigestMismatch));
}

#[test]
fn a_file_giving_one_sequence_two_bodies_is_refused() {
    // Once each, and every one. A file naming a body twice would build one
    // sequence over the top of itself and leave the other empty, and both
    // halves of that are a project no editor produced.
    //
    // The fixture gets a note in the *inner* sequence, which is what makes the
    // splice findable: markers are the last thing in a body, so the four bytes
    // after the note's text are the next body's header index.
    let mut project = nested();
    let inner = project
        .sequences()
        .iter()
        .map(|(id, _)| id)
        .nth(1)
        .expect("two sequences");
    project
        .apply(
            inner,
            Edit::AddMarker {
                at: Instant::new(10, RATE),
                text: "innermost".into(),
            },
        )
        .expect("a note");
    let file = encode(&project).expect("an encoding");
    let at = file
        .windows(9)
        .position(|window| window == b"innermost")
        .expect("the note is in the file");
    let next = at + 9;
    // The bodies are written innermost first, so the note ends the *first* of
    // them and the outer sequence -- header index nought -- follows.
    assert_eq!(
        &file[next..next + 4],
        &0_u32.to_le_bytes(),
        "the second body names the outer sequence"
    );
    let mut twice = file.clone();
    twice[next..next + 4].copy_from_slice(&1_u32.to_le_bytes());
    assert_eq!(decode(&resealed(twice)), Err(IoStatus::SequenceBodyTwice));
}

/// A project whose one clip runs on a ramp: full speed, down to a quarter.
///
/// The first keyframe sits at tick 777 so that the file can be searched for it
/// without the pattern colliding with anything else in the payload.
fn ramped() -> Project {
    use media_editor_model::curve::{Curve, Interpolation, Keyframe};

    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"footage"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    let ramp = Curve::new(std::vec![
        Keyframe::new(
            Instant::new(777, RATE),
            Rational::ONE,
            Interpolation::Linear
        )
        .expect("a keyframe"),
        Keyframe::new(
            Instant::new(1_200, RATE),
            Rational::new(1, 4).expect("a quarter"),
            Interpolation::Linear
        )
        .expect("a keyframe"),
    ])
    .expect("a curve");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(
                    Clip::new(media, 0, frames(2_000))
                        .expect("a clip")
                        .with_ramp(ramp)
                        .expect("a ramp"),
                ),
            },
        )
        .expect("a clip");
    project.forget_history();
    project
}

#[test]
fn a_ramp_survives_the_file() {
    let back = round_tripped(&ramped());
    assert_eq!(back, ramped(), "the ramp did not come back as it went in");
}

#[test]
fn a_file_whose_ramp_turns_around_is_refused() {
    // Through the model's own door, like everything else: a file cannot hold a
    // clip no sequence of edits could produce. The second keyframe's value is
    // negated, which makes the ramp cross nought part way along -- and a ramp
    // that turns around reads part of its media twice.
    let file = encode(&ramped()).expect("an encoding");
    // The second keyframe: its instant, then its value as a numerator over a
    // denominator, then the byte saying how it leaves.
    let at = file
        .windows(8)
        .position(|window| window == 1_200_i64.to_le_bytes())
        .expect("the keyframe is in the file");
    let mut turning = file.clone();
    turning[at + 8..at + 16].copy_from_slice(&(-1_i64).to_le_bytes());
    assert_eq!(
        decode(&resealed(turning)),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::SpeedRampChangesDirection
        ))
    );
}

#[test]
fn a_file_whose_ramp_has_no_keyframes_is_refused() {
    // A ramp is a curve and a curve holds at least one keyframe, so the tag
    // above the count is what says whether there is a ramp at all. A count of
    // nought under a ramp tag is a file saying both things at once.
    let file = encode(&ramped()).expect("an encoding");
    // The ramp's tag, then its count of two keyframes, then the first
    // keyframe's instant -- which is what pins the pattern to this curve
    // rather than to any other in the payload.
    let mut wanted = std::vec::Vec::new();
    wanted.push(3_u8);
    wanted.extend_from_slice(&2_u32.to_le_bytes());
    wanted.extend_from_slice(&777_i64.to_le_bytes());
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the ramp is in the file");
    let mut empty = file.clone();
    empty[at + 1..at + 5].copy_from_slice(&0_u32.to_le_bytes());
    assert_eq!(
        decode(&resealed(empty)),
        Err(IoStatus::RampWithoutKeyframes)
    );
}

#[test]
fn a_file_whose_ramp_eases_is_refused() {
    // The refusal the model makes when a ramp is set, made again here. The
    // first keyframe's interpolation byte is changed from linear to an ease,
    // and the four handle rationals it would then want are spliced in after
    // it -- so this is a *well-formed* eased curve that the model refuses on
    // its meaning rather than a truncated one it refuses on its shape.
    let file = encode(&ramped()).expect("an encoding");
    let at = file
        .windows(8)
        .position(|window| window == 777_i64.to_le_bytes())
        .expect("the keyframe is in the file");
    // The instant, sixteen bytes of value, then the byte saying how it leaves.
    let how = at + 8 + 16;
    assert_eq!(file[how], 2, "the keyframe runs straight to the next");
    let mut eased = file[..how].to_vec();
    eased.push(3);
    for handle in [(1_i64, 3_i64), (0, 1), (2, 3), (1, 1)] {
        eased.extend_from_slice(&handle.0.to_le_bytes());
        eased.extend_from_slice(&handle.1.to_le_bytes());
    }
    eased.extend_from_slice(&file[how + 1..]);
    assert_eq!(
        decode(&resealed(eased)),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::EaseHasNoExactArea
        ))
    );
}

/// A project whose one clip carries two notes.
fn noted() -> Project {
    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"footage"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 0, frames(2_000)).expect("a clip")),
            },
        )
        .expect("a clip");
    for (tick, text) in [(64_i64, "eyeline"), (512, "boom in shot")] {
        project
            .apply(
                sequence,
                Edit::AddClipMarker {
                    track: 0,
                    index: 0,
                    at: Instant::new(tick, RATE),
                    text: text.into(),
                },
            )
            .expect("a note");
    }
    project.forget_history();
    project
}

#[test]
fn a_shots_notes_survive_the_file() {
    assert_eq!(round_tripped(&noted()), noted());
}

#[test]
fn a_file_giving_a_shot_two_notes_at_one_offset_is_refused() {
    // Through the model's own door, one note at a time, so a file cannot hold
    // a clip no sequence of edits could produce.
    let file = encode(&noted()).expect("an encoding");
    let at = file
        .windows(8)
        .position(|window| window == 512_i64.to_le_bytes())
        .expect("the note is in the file");
    let mut twice = file.clone();
    twice[at..at + 8].copy_from_slice(&64_i64.to_le_bytes());
    assert_eq!(
        decode(&resealed(twice)),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::MarkerExists
        ))
    );
}

#[test]
fn a_file_giving_a_shot_a_note_before_it_starts_is_refused() {
    let file = encode(&noted()).expect("an encoding");
    let at = file
        .windows(8)
        .position(|window| window == 64_i64.to_le_bytes())
        .expect("the note is in the file");
    let mut early = file.clone();
    early[at..at + 8].copy_from_slice(&(-1_i64).to_le_bytes());
    assert_eq!(
        decode(&resealed(early)),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::MarkerBeforeStart
        ))
    );
}

#[test]
fn a_file_giving_a_shot_more_notes_than_the_bound_is_refused() {
    // The bound is the *clip's*, which is three orders of magnitude below the
    // sequence's. Reading a clip's list against the sequence's bound would let
    // a file talk its way past the smaller one (R-11.2), which is exactly what
    // this splices: a count of nine, which a sequence would accept happily.
    let file = encode(&noted()).expect("an encoding");
    let mut wanted = std::vec::Vec::new();
    wanted.extend_from_slice(&2_u32.to_le_bytes());
    wanted.extend_from_slice(&64_i64.to_le_bytes());
    let at = file
        .windows(wanted.len())
        .position(|window| window == wanted.as_slice())
        .expect("the note count is in the file");
    let mut many = file.clone();
    many[at..at + 4].copy_from_slice(
        &u32::try_from(media_editor_model::MAX_MARKERS_PER_CLIP + 1)
            .expect("a count")
            .to_le_bytes(),
    );
    assert_eq!(decode(&resealed(many)), Err(IoStatus::TooMany));
}

/// A project holding one captioned recording.
fn transcribed() -> Project {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let asset = MediaAsset::new(
        Digest::of(b"an interview"),
        RATE,
        Duration::new(1000, RATE).expect("a length"),
    )
    .expect("an asset")
    .with_captions(std::vec![
        media_editor_model::caption::Caption::new(0, 10, 0, "did you get that").expect("a caption"),
        media_editor_model::caption::Caption::new(5, 15, 1, "most of it").expect("a caption"),
        media_editor_model::caption::Caption::new(10, 20, 0, "good").expect("a caption"),
    ])
    .expect("a transcript");
    let media = project.add_media(asset).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(
                    Clip::new(media, 0, Duration::new(20, RATE).expect("a length"))
                        .expect("a clip"),
                ),
            },
        )
        .expect("an insert");
    project
}

#[test]
fn a_transcript_survives_a_round_trip() {
    let project = transcribed();
    let file = encode(&project).expect("an encoding");
    let back = decode(&file).expect("a decoding");
    // The media and the programme, rather than the whole project: a file does
    // not carry the edit journal, so the two differ in the history and in
    // nothing else.
    assert_eq!(back.media(), project.media());
    let sequence = project.sequences().iter().next().expect("a sequence").0;
    assert_eq!(
        back.sequence(sequence).expect("a sequence"),
        project.sequence(sequence).expect("a sequence")
    );
    // Field for field as well as as a whole, so a decoder that lost the voice
    // and kept the words would be caught by name.
    let (_, asset) = back.media().iter().next().expect("an asset");
    let captions = asset.captions();
    assert_eq!(captions.len(), 3);
    assert_eq!(captions[0].text(), "did you get that");
    assert_eq!((captions[0].from(), captions[0].to()), (0, 10));
    assert_eq!(captions[1].voice(), 1, "the second speaker survived");
    assert_eq!(captions[2].text(), "good");
    // And re-encoding gives the same bytes, so nothing about the transcript
    // depends on the order it came out of the file.
    assert_eq!(encode(&back).expect("an encoding"), file);
}

#[test]
fn a_file_holding_a_caption_no_editor_could_make_is_refused() {
    // A hostile file, resealed so that the digest cannot be what refuses it.
    // The reader comes through the model's own constructor, so every bound the
    // model puts on a transcript a file has to pass too (R-11.2).
    let file = encode(&transcribed()).expect("an encoding");
    // The first caption's in and out points: 0 and 10, eight bytes each,
    // immediately after the asset's digest and its own count of three.
    let at = file
        .windows(16)
        .position(|window| {
            window[..8] == 0_i64.to_le_bytes() && window[8..] == 10_i64.to_le_bytes()
        })
        .expect("the first caption's range is in the file");

    // A caption that ends before it begins.
    let mut backwards = file.clone();
    backwards[at..at + 8].copy_from_slice(&20_i64.to_le_bytes());
    let sealed = Digest::of(&backwards[HEADER_BYTES..]);
    backwards[16..48].copy_from_slice(sealed.bytes());
    assert_eq!(
        decode(&backwards),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::EmptyCaption
        )),
        "a file holding a backwards caption has to be refused"
    );

    // Two captions of one voice over the same tick: the second speaker's voice
    // changed to the first's, which makes it overlap the caption above it.
    let mut doubled = file.clone();
    let voice = doubled
        .windows(16)
        .position(|window| {
            window[..8] == 5_i64.to_le_bytes() && window[8..] == 15_i64.to_le_bytes()
        })
        .expect("the second caption's range is in the file")
        + 16;
    assert_eq!(doubled[voice], 1, "the second caption is the second voice");
    doubled[voice] = 0;
    let sealed = Digest::of(&doubled[HEADER_BYTES..]);
    doubled[16..48].copy_from_slice(sealed.bytes());
    assert_eq!(
        decode(&doubled),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::CaptionsOverlap
        )),
        "one voice cannot say two things at once, in a file either"
    );
}
