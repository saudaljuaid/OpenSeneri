// SPDX-License-Identifier: GPL-3.0-only
//! Conforming: the round trip, and everywhere it stops.
//!
//! One theorem and the complete list of its exceptions. The theorem is that a
//! sequence which exports with nothing left behind comes back **equal** — by
//! `PartialEq` on the whole value, so a field nobody thought to compare is
//! still compared. The exceptions are the five fields of `LeftBehind`, and
//! each has a pair of tests here: one that it is reported, and one that a
//! sequence carrying it really does come back different. A reported loss that
//! turns out to be no loss would be a warning nobody should heed.

use media_editor_core::{Digest, Duration, Instant, Rational, Timebase, Timecode};
use media_editor_io::conform::{self, LeftBehind};
use media_editor_io::edl;
use media_editor_io::{Channel, IoStatus, Transition};
use media_editor_model::curve::{Curve, Interpolation, Keyframe};
use media_editor_model::track::Fader;
use media_editor_model::{
    Clip, Edit, Item, MediaAsset, MediaId, Project, SequenceId, TrackKind, track,
};

const RATE: Timebase = Timebase::FILM_24;

/// One hour of twenty-four frames, which is where a programme starts.
const HOUR: i64 = 86_400;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a non-negative length")
}

fn at(frame: i64) -> Timecode {
    Timecode::from_frame_number(frame, 24, false).expect("a position inside the day")
}

fn start() -> Timecode {
    at(HOUR)
}

fn clip(media: MediaId, source: i64, length: i64) -> Item {
    Item::Clip(Clip::new(media, source, frames(length)).expect("a clip"))
}

fn insert(project: &mut Project, sequence: SequenceId, track: usize, index: usize, item: Item) {
    project
        .apply(sequence, Edit::InsertItem { track, index, item })
        .expect("an item the track accepts");
}

/// Two assets, three tracks, a gap on two of them, and a dissolve.
///
/// Every feature the format *can* carry is on it, so a round trip that
/// preserves this one is not preserving a special case.
fn fixture() -> (Project, SequenceId, MediaId, MediaId) {
    let mut project = Project::new();
    let first = project
        .add_media(MediaAsset::new(Digest::of(b"first"), RATE, frames(50_000)).expect("an asset"))
        .expect("room");
    let second = project
        .add_media(MediaAsset::new(Digest::of(b"second"), RATE, frames(50_000)).expect("an asset"))
        .expect("room");

    let sequence = project.add_sequence(RATE).expect("room");
    for (index, kind) in [
        (0, TrackKind::Video),
        (1, TrackKind::Audio),
        (2, TrackKind::Audio),
    ] {
        project
            .apply(sequence, Edit::AddTrack { index, kind })
            .expect("a track");
    }

    // Picture: a clip, a hole, a clip, and a clip dissolved into.
    insert(&mut project, sequence, 0, 0, clip(first, 240, 96));
    insert(
        &mut project,
        sequence,
        0,
        1,
        Item::gap(frames(24)).expect("a gap"),
    );
    insert(&mut project, sequence, 0, 2, clip(second, 480, 120));
    insert(&mut project, sequence, 0, 3, clip(first, 600, 72));
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: track::Transition::new(3, frames(24)).expect("a dissolve"),
            },
        )
        .expect("a dissolve the track accepts");

    // Sound: one full-length lane, and one that comes in late.
    insert(&mut project, sequence, 1, 0, clip(first, 240, 216));
    insert(
        &mut project,
        sequence,
        2,
        0,
        Item::gap(frames(48)).expect("a gap"),
    );
    insert(&mut project, sequence, 2, 1, clip(second, 0, 168));

    project.forget_history();
    (project, sequence, first, second)
}

/// Export, write, parse, import: the whole path a file takes.
fn round_trip(project: &Project, sequence: SequenceId) -> (Project, SequenceId, LeftBehind) {
    let conformed = conform::export(project, sequence, start()).expect("a list");
    let text = edl::write(conformed.list()).expect("a written list");
    let read = edl::parse(&text).expect("a list that parses");
    let mut into = Project::new();
    for (_, asset) in project.media().iter() {
        into.add_media(asset.clone()).expect("room");
    }
    let landed = conform::import(&mut into, &read, RATE, start()).expect("a sequence");
    (into, landed, conformed.left_behind())
}

#[test]
fn a_sequence_that_loses_nothing_comes_back_equal() {
    let (project, sequence, _, _) = fixture();
    let (into, landed, left) = round_trip(&project, sequence);
    assert!(left.is_empty(), "the fixture is meant to lose nothing");
    assert_eq!(
        into.sequence(landed).expect("the sequence"),
        project.sequence(sequence).expect("the sequence"),
        "a lossless export has to come back as the same value, field for field"
    );
}

#[test]
fn nothing_left_behind_is_every_field_at_zero() {
    let (project, sequence, _, _) = fixture();
    let conformed = conform::export(&project, sequence, start()).expect("a list");
    let left = conformed.left_behind();
    assert_eq!(left.grades(), 0);
    assert_eq!(left.automation(), 0);
    assert_eq!(left.faders(), 0);
    assert_eq!(left.silent_tracks(), 0);
    assert_eq!(left.trailing_gaps(), 0);
    assert_eq!(left, LeftBehind::default());
}

#[test]
fn the_events_are_the_clips_and_not_the_items() {
    let (project, sequence, _, _) = fixture();
    let conformed = conform::export(&project, sequence, start()).expect("a list");
    // Five clips across three tracks; the two gaps write nothing, because a
    // hole in the record timecode is how the format says black.
    assert_eq!(conformed.list().events().len(), 5);
    assert_eq!(
        conformed.list().channels(),
        [Channel::Video, Channel::Audio(1), Channel::Audio(2)]
    );
}

#[test]
fn a_clip_writes_the_source_it_uses() {
    let (project, sequence, _, _) = fixture();
    let conformed = conform::export(&project, sequence, start()).expect("a list");
    let event = &conformed.list().events()[0];
    assert_eq!(event.source_in(), at(240));
    // Exclusive: ninety-six frames from 240 ends at 336, and the last frame
    // used is 335. An importer reading this as inclusive makes the clip
    // ninety-seven frames long.
    assert_eq!(event.source_out(), at(336));
    assert_eq!(event.record_in(), at(HOUR));
    assert_eq!(event.record_out(), at(HOUR + 96));
}

#[test]
fn a_gap_is_a_hole_in_the_record_timecode() {
    let (project, sequence, _, _) = fixture();
    let conformed = conform::export(&project, sequence, start()).expect("a list");
    let events = conformed.list().events();
    assert_eq!(events[0].record_out(), at(HOUR + 96));
    // Twenty-four frames of nothing, said by the next event beginning later
    // than the last one ended.
    assert_eq!(events[1].record_in(), at(HOUR + 120));
}

#[test]
fn a_leading_gap_survives_the_round_trip() {
    let (project, sequence, _, _) = fixture();
    let (into, landed, _) = round_trip(&project, sequence);
    let track = into
        .sequence(landed)
        .expect("the sequence")
        .track(2)
        .expect("the second sound track");
    assert!(matches!(track.item(0).expect("an item"), Item::Gap(_)));
    assert_eq!(
        track.item(0).expect("an item").duration().ticks(),
        48,
        "a channel that comes in late comes back in late"
    );
}

#[test]
fn a_dissolve_takes_its_opening_off_the_outgoing_event() {
    let (project, sequence, _, _) = fixture();
    let conformed = conform::export(&project, sequence, start()).expect("a list");
    let events = conformed.list().events();
    // The outgoing clip runs to the cut at 240 in the model. On the record it
    // stops twelve frames earlier, where the dissolve begins, and the frames
    // between are stated by the dissolve rather than by the event.
    assert_eq!(events[1].record_out(), at(HOUR + 228));
    assert_eq!(events[2].record_in(), at(HOUR + 228));
    assert_eq!(events[2].transition(), Transition::Dissolve(24));
    // And the incoming one starts twelve frames early into its handles.
    assert_eq!(events[2].source_in(), at(588));
}

#[test]
fn a_dissolve_gives_the_outgoing_clip_its_tail_back() {
    let (project, sequence, _, _) = fixture();
    let (into, landed, _) = round_trip(&project, sequence);
    let track = into
        .sequence(landed)
        .expect("the sequence")
        .track(0)
        .expect("the picture track");
    assert_eq!(
        track.item(2).expect("an item").duration().ticks(),
        120,
        "the twelve frames the dissolve took off the event go back on the clip"
    );
    assert_eq!(track.transitions().len(), 1);
    assert_eq!(track.transitions()[0].boundary(), 3);
    assert_eq!(track.transitions()[0].duration().ticks(), 24);
}

#[test]
fn an_odd_dissolve_leans_the_same_way_after_a_round_trip() {
    // Twenty-five frames cannot be centred on a frame boundary, so the model
    // opens twelve before the cut and thirteen after. The list has to lean the
    // same way or the picture moves by a frame.
    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"odd"), RATE, frames(9_000)).expect("an asset"))
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
    insert(&mut project, sequence, 0, 0, clip(media, 100, 48));
    insert(&mut project, sequence, 0, 1, clip(media, 500, 48));
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: track::Transition::new(1, frames(25)).expect("a dissolve"),
            },
        )
        .expect("a dissolve the track accepts");
    project.forget_history();

    let conformed = conform::export(&project, sequence, start()).expect("a list");
    let events = conformed.list().events();
    assert_eq!(
        events[0].record_out(),
        at(HOUR + 36),
        "twelve before the cut at forty-eight, not thirteen"
    );
    assert_eq!(events[1].source_in(), at(488));

    let (into, landed, left) = round_trip(&project, sequence);
    assert!(left.is_empty());
    assert_eq!(
        into.sequence(landed).expect("the sequence"),
        project.sequence(sequence).expect("the sequence")
    );
}

#[test]
fn the_record_timecode_is_the_order_and_the_event_number_is_not() {
    let (project, sequence, _, _) = fixture();
    let conformed = conform::export(&project, sequence, start()).expect("a list");
    // The same events, handed over backwards *and renumbered to agree with the
    // order they are handed over in* -- so the numbers are a complete,
    // self-consistent account of a cut that runs the other way. An importer
    // that trusted them would build the programme inside out; one that reads
    // the record timecode does not notice.
    //
    // Reversing without renumbering proves nothing: the numbers would still
    // ascend with the record, so sorting by either gives the same answer.
    let mut reversed: Vec<_> = conformed.list().events().to_vec();
    reversed.reverse();
    let renumbered: Vec<_> = reversed
        .iter()
        .enumerate()
        .map(|(position, event)| {
            let number = u32::try_from(position + 1).expect("a small count");
            let built = edl::event(
                number,
                event.reel(),
                event.channel(),
                event.transition(),
                (event.source_in(), event.source_out()),
                (event.record_in(), event.record_out()),
            )
            .expect("an event");
            match event.from_clip_name() {
                Some(name) => edl::named(built, name),
                None => built,
            }
        })
        .collect();
    assert_eq!(renumbered[0].number(), 1);
    assert_eq!(
        renumbered[0].record_in(),
        conformed.list().events()[4].record_in(),
        "event one is now the last clip in the programme"
    );
    let shuffled = edl::list("SHUFFLED", renumbered).expect("a list");
    let mut into = Project::new();
    for (_, asset) in project.media().iter() {
        into.add_media(asset.clone()).expect("room");
    }
    let landed = conform::import(&mut into, &shuffled, RATE, start()).expect("a sequence");
    assert_eq!(
        into.sequence(landed).expect("the sequence"),
        project.sequence(sequence).expect("the sequence")
    );
}

#[test]
fn an_edl_that_starts_at_an_hour_does_not_import_an_hour_of_black() {
    let (project, sequence, _, _) = fixture();
    let (into, landed, _) = round_trip(&project, sequence);
    let track = into
        .sequence(landed)
        .expect("the sequence")
        .track(0)
        .expect("the picture track");
    assert!(
        matches!(track.item(0).expect("an item"), Item::Clip(_)),
        "the programme starts at its first frame, wherever the tape started"
    );
}

#[test]
fn reading_the_hour_as_a_position_puts_an_hour_of_black_at_the_head() {
    // The other half of the same fact, and the reason the record start is an
    // argument rather than a guess. Told the wrong origin, the importer is not
    // wrong about anything -- it lays the cut out exactly where it was told
    // the programme begins, which is an hour after zero.
    let (project, sequence, _, _) = fixture();
    let conformed = conform::export(&project, sequence, start()).expect("a list");
    let text = edl::write(conformed.list()).expect("a written list");
    let read = edl::parse(&text).expect("a list that parses");
    let mut into = Project::new();
    for (_, asset) in project.media().iter() {
        into.add_media(asset.clone()).expect("room");
    }
    let landed = conform::import(&mut into, &read, RATE, at(0)).expect("a sequence");
    let track = into
        .sequence(landed)
        .expect("the sequence")
        .track(0)
        .expect("the picture track");
    let head = track.item(0).expect("an item");
    assert!(matches!(head, Item::Gap(_)));
    assert_eq!(head.duration().ticks(), HOUR);
}

#[test]
fn a_drop_frame_list_counts_the_frames_it_skips() {
    // Thirty-thousand over a thousand and one, where the label at ten minutes
    // has had eighteen numbers taken out of it. The round trip is over frame
    // numbers, so it either survives the counting or it does not.
    let ntsc = Timebase::NTSC_30;
    let mut project = Project::new();
    let media = project
        .add_media(
            MediaAsset::new(
                Digest::of(b"ntsc"),
                ntsc,
                Duration::new(200_000, ntsc).expect("a length"),
            )
            .expect("an asset"),
        )
        .expect("room");
    let sequence = project.add_sequence(ntsc).expect("room");
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
                    Clip::new(media, 0, Duration::new(30_000, ntsc).expect("a length"))
                        .expect("a clip"),
                ),
            },
        )
        .expect("a clip");
    project.forget_history();

    let origin = Timecode::from_frame_number(107_892, 30, true).expect("a drop-frame label");
    let conformed = conform::export(&project, sequence, origin).expect("a list");
    assert!(
        conformed.list().events()[0].record_in().is_drop_frame(),
        "the record start decides the counting and every label follows it"
    );
    let text = edl::write(conformed.list()).expect("a written list");
    assert!(text.contains("FCM: DROP FRAME"));
    let read = edl::parse(&text).expect("a list that parses");
    let mut into = Project::new();
    for (_, asset) in project.media().iter() {
        into.add_media(asset.clone()).expect("room");
    }
    let landed = conform::import(&mut into, &read, ntsc, origin).expect("a sequence");
    assert_eq!(
        into.sequence(landed).expect("the sequence"),
        project.sequence(sequence).expect("the sequence")
    );
}

#[test]
fn a_reel_name_is_the_first_eight_characters_of_the_source_digest() {
    let (project, sequence, first, _) = fixture();
    let conformed = conform::export(&project, sequence, start()).expect("a list");
    let digest = project
        .media()
        .get(first)
        .expect("the asset")
        .digest()
        .to_string();
    let event = &conformed.list().events()[0];
    assert_eq!(event.reel(), &digest[..8]);
    assert_eq!(
        event.from_clip_name(),
        Some(digest.as_str()),
        "the whole of it goes in the comment, because eight characters is not an identity"
    );
}

#[test]
fn two_sources_sharing_eight_characters_are_refused() {
    // A reel name holds thirty-two bits of a digest. Two sources that agree in
    // those bits are one reel name for two pieces of media, and writing them
    // both would produce a list that cannot be read back correctly by anything
    // -- including this.
    let mut project = Project::new();
    let mut left = [0_u8; 32];
    left[0..4].copy_from_slice(&[0xAB, 0xCD, 0xEF, 0x01]);
    let mut right = left;
    right[31] = 0xFF;
    let one = project
        .add_media(MediaAsset::new(Digest::new(left), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let two = project
        .add_media(MediaAsset::new(Digest::new(right), RATE, frames(9_000)).expect("an asset"))
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
    insert(&mut project, sequence, 0, 0, clip(one, 0, 48));
    insert(&mut project, sequence, 0, 1, clip(two, 0, 48));
    assert_eq!(
        conform::export(&project, sequence, start()),
        Err(IoStatus::ConformReelCollision)
    );
}

#[test]
fn the_same_source_twice_is_one_reel_and_not_a_collision() {
    let (project, sequence, _, _) = fixture();
    // The picture track uses the first asset twice. If sharing a reel name
    // were the test, this would refuse; it is sharing a name while naming
    // different media.
    assert!(conform::export(&project, sequence, start()).is_ok());
}

#[test]
fn a_second_picture_track_is_refused() {
    let (mut project, sequence, first, _) = fixture();
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 1,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    insert(&mut project, sequence, 1, 0, clip(first, 0, 48));
    assert_eq!(
        conform::export(&project, sequence, start()),
        Err(IoStatus::ConformManyVideoTracks),
        "one video channel cannot say which of two pictures is on top"
    );
}

#[test]
fn sound_before_picture_is_refused() {
    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"one"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    for (index, kind) in [(0, TrackKind::Audio), (1, TrackKind::Video)] {
        project
            .apply(sequence, Edit::AddTrack { index, kind })
            .expect("a track");
    }
    insert(&mut project, sequence, 0, 0, clip(media, 0, 48));
    insert(&mut project, sequence, 1, 0, clip(media, 0, 48));
    assert_eq!(
        conform::export(&project, sequence, start()),
        Err(IoStatus::ConformTracksNotOrdered),
        "a list names channels and not an order, so the order has to be the one it rebuilds"
    );
}

#[test]
fn a_sequence_with_no_clip_anywhere_is_not_a_list() {
    let mut project = Project::new();
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
    assert_eq!(
        conform::export(&project, sequence, start()),
        Err(IoStatus::EdlNoEvents)
    );
}

#[test]
fn a_grade_is_left_behind_and_the_sequence_comes_back_different() {
    let (mut project, sequence, _, _) = fixture();
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
    let conformed = conform::export(&project, sequence, start()).expect("a list");
    assert_eq!(conformed.left_behind().grades(), 1);
    assert!(!conformed.left_behind().is_empty());

    let (into, landed, _) = round_trip(&project, sequence);
    assert_ne!(
        into.sequence(landed).expect("the sequence"),
        project.sequence(sequence).expect("the sequence"),
        "a reported loss that changed nothing would be a warning nobody should heed"
    );
}

#[test]
fn automation_is_left_behind_and_the_sequence_comes_back_different() {
    let (mut project, sequence, _, _) = fixture();
    let opacity = Curve::new(
        [
            Keyframe::new(Instant::new(0, RATE), Rational::ONE, Interpolation::Linear)
                .expect("a keyframe"),
            Keyframe::new(
                Instant::new(96, RATE),
                Rational::ZERO,
                Interpolation::Linear,
            )
            .expect("a keyframe"),
        ]
        .to_vec(),
    )
    .expect("a curve");
    project
        .apply(
            sequence,
            Edit::SetTrackOpacity {
                track: 0,
                opacity: Some(opacity),
            },
        )
        .expect("a curve the track accepts");
    let conformed = conform::export(&project, sequence, start()).expect("a list");
    assert_eq!(conformed.left_behind().automation(), 1);

    let (into, landed, _) = round_trip(&project, sequence);
    assert_ne!(
        into.sequence(landed).expect("the sequence"),
        project.sequence(sequence).expect("the sequence")
    );
}

#[test]
fn a_fader_is_left_behind_and_the_sequence_comes_back_different() {
    let (mut project, sequence, _, _) = fixture();
    project
        .apply(
            sequence,
            Edit::SetTrackFader {
                track: 1,
                fader: Fader::at(Rational::from_integer(-6)).expect("a level"),
            },
        )
        .expect("a fader");
    let conformed = conform::export(&project, sequence, start()).expect("a list");
    assert_eq!(conformed.left_behind().faders(), 1);

    let (into, landed, _) = round_trip(&project, sequence);
    assert_ne!(
        into.sequence(landed).expect("the sequence"),
        project.sequence(sequence).expect("the sequence")
    );
}

#[test]
fn mute_is_a_fader_and_is_left_behind_too() {
    let (mut project, sequence, _, _) = fixture();
    project
        .apply(
            sequence,
            Edit::SetTrackFader {
                track: 1,
                fader: Fader::MUTED,
            },
        )
        .expect("a fader");
    let conformed = conform::export(&project, sequence, start()).expect("a list");
    assert_eq!(
        conformed.left_behind().faders(),
        1,
        "a muted track exports its events, and the list has no way to say they are off"
    );
}

#[test]
fn a_track_with_nothing_on_it_is_left_behind_and_does_not_come_back() {
    let (mut project, sequence, _, _) = fixture();
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 3,
                kind: TrackKind::Audio,
            },
        )
        .expect("a track");
    let conformed = conform::export(&project, sequence, start()).expect("a list");
    assert_eq!(conformed.left_behind().silent_tracks(), 1);

    let (into, landed, _) = round_trip(&project, sequence);
    assert_eq!(
        into.sequence(landed).expect("the sequence").track_count(),
        3,
        "a track that writes no event cannot be said to exist"
    );
    assert_ne!(
        into.sequence(landed).expect("the sequence"),
        project.sequence(sequence).expect("the sequence")
    );
}

#[test]
fn a_trailing_gap_is_left_behind_and_does_not_come_back() {
    let (mut project, sequence, _, _) = fixture();
    insert(
        &mut project,
        sequence,
        1,
        1,
        Item::gap(frames(120)).expect("a gap"),
    );
    let conformed = conform::export(&project, sequence, start()).expect("a list");
    assert_eq!(
        conformed.left_behind().trailing_gaps(),
        1,
        "a list ends at its last event, so black after the last sound is not sayable"
    );

    let (into, landed, _) = round_trip(&project, sequence);
    assert_eq!(
        into.sequence(landed)
            .expect("the sequence")
            .track(1)
            .expect("a track")
            .len(),
        1
    );
    assert_ne!(
        into.sequence(landed).expect("the sequence"),
        project.sequence(sequence).expect("the sequence")
    );
}

/// A list built by hand, so that the importer can be handed something no
/// export would produce.
fn hand_built(events: Vec<media_editor_io::Event>) -> media_editor_io::EditDecisionList {
    edl::list("BY HAND", events).expect("a list")
}

fn library() -> (Project, MediaId, Digest) {
    let mut project = Project::new();
    let digest = Digest::of(b"only");
    let media = project
        .add_media(MediaAsset::new(digest, RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    (project, media, digest)
}

fn plain(
    number: u32,
    reel: &str,
    transition: Transition,
    source: i64,
    record: (i64, i64),
) -> media_editor_io::Event {
    edl::event(
        number,
        reel,
        Channel::Video,
        transition,
        (at(source), at(source + record.1 - record.0)),
        (at(record.0), at(record.1)),
    )
    .expect("an event")
}

#[test]
fn an_event_with_no_source_comment_is_refused() {
    let (mut project, _, digest) = library();
    let reel = digest.to_string()[..8].to_owned();
    let list = hand_built(vec![plain(1, &reel, Transition::Cut, 0, (HOUR, HOUR + 48))]);
    assert_eq!(
        conform::import(&mut project, &list, RATE, start()),
        Err(IoStatus::ConformNoSourceDigest),
        "eight characters is not an identity, and inventing one is worse than refusing"
    );
}

#[test]
fn a_comment_that_is_not_a_digest_is_refused() {
    let (mut project, _, digest) = library();
    let reel = digest.to_string()[..8].to_owned();
    let list = hand_built(vec![edl::named(
        plain(1, &reel, Transition::Cut, 0, (HOUR, HOUR + 48)),
        "shot_04a.mov",
    )]);
    assert_eq!(
        conform::import(&mut project, &list, RATE, start()),
        Err(IoStatus::ConformNoSourceDigest)
    );
}

#[test]
fn a_reel_that_disagrees_with_its_source_is_refused() {
    let (mut project, _, digest) = library();
    let list = hand_built(vec![edl::named(
        plain(1, "TAPE01", Transition::Cut, 0, (HOUR, HOUR + 48)),
        &digest.to_string(),
    )]);
    assert_eq!(
        conform::import(&mut project, &list, RATE, start()),
        Err(IoStatus::ConformReelDisagreesWithSource),
        "two statements about one fact, and a reader that picks one is guessing"
    );
}

#[test]
fn a_source_the_library_does_not_hold_is_refused() {
    let (mut project, _, _) = library();
    let absent = Digest::of(b"never opened");
    let reel = absent.to_string()[..8].to_owned();
    let list = hand_built(vec![edl::named(
        plain(1, &reel, Transition::Cut, 0, (HOUR, HOUR + 48)),
        &absent.to_string(),
    )]);
    assert_eq!(
        conform::import(&mut project, &list, RATE, start()),
        Err(IoStatus::ConformUnknownSource),
        "conforming is not importing media"
    );
}

fn one_source(
    number: u32,
    transition: Transition,
    source: i64,
    record: (i64, i64),
) -> media_editor_io::Event {
    let digest = Digest::of(b"only");
    let reel = digest.to_string()[..8].to_owned();
    edl::named(
        plain(number, &reel, transition, source, record),
        &digest.to_string(),
    )
}

#[test]
fn a_b_event_is_refused() {
    let (mut project, _, digest) = library();
    let reel = digest.to_string()[..8].to_owned();
    let event = edl::event(
        1,
        &reel,
        Channel::VideoAndAudio,
        Transition::Cut,
        (at(0), at(48)),
        (at(HOUR), at(HOUR + 48)),
    )
    .expect("an event");
    let list = hand_built(vec![edl::named(event, &digest.to_string())]);
    assert_eq!(
        conform::import(&mut project, &list, RATE, start()),
        Err(IoStatus::ConformChannelNotSeparate),
        "which sound tracks a B event means is a convention, not a statement"
    );
}

#[test]
fn a_wipe_is_refused() {
    let (mut project, _, _) = library();
    let list = hand_built(vec![
        one_source(1, Transition::Cut, 0, (HOUR, HOUR + 48)),
        one_source(
            2,
            Transition::Wipe {
                pattern: 1,
                frames: 24,
            },
            100,
            (HOUR + 48, HOUR + 96),
        ),
    ]);
    assert_eq!(
        conform::import(&mut project, &list, RATE, start()),
        Err(IoStatus::ConformWipeUnsupported),
        "this build has no shape to wipe with, and a wipe read as a dissolve is a different picture"
    );
}

#[test]
fn overlapping_events_are_refused() {
    let (mut project, _, _) = library();
    let list = hand_built(vec![
        one_source(1, Transition::Cut, 0, (HOUR, HOUR + 48)),
        one_source(2, Transition::Cut, 100, (HOUR + 24, HOUR + 96)),
    ]);
    assert_eq!(
        conform::import(&mut project, &list, RATE, start()),
        Err(IoStatus::ConformEventsOverlap),
        "a track's items tile it, so two events over one frame describe no track"
    );
}

#[test]
fn a_dissolve_on_the_first_event_is_refused() {
    let (mut project, _, _) = library();
    let list = hand_built(vec![one_source(
        1,
        Transition::Dissolve(24),
        100,
        (HOUR, HOUR + 48),
    )]);
    assert_eq!(
        conform::import(&mut project, &list, RATE, start()),
        Err(IoStatus::ConformDissolveFromNothing)
    );
}

#[test]
fn a_dissolve_after_a_hole_is_refused() {
    let (mut project, _, _) = library();
    let list = hand_built(vec![
        one_source(1, Transition::Cut, 0, (HOUR, HOUR + 48)),
        one_source(2, Transition::Dissolve(24), 100, (HOUR + 72, HOUR + 144)),
    ]);
    assert_eq!(
        conform::import(&mut project, &list, RATE, start()),
        Err(IoStatus::ConformDissolveFromNothing),
        "a dissolve to a clip of black is a dissolve to a clip, and a hole is not one"
    );
}

#[test]
fn a_dissolve_longer_than_its_event_is_refused() {
    let (mut project, _, _) = library();
    let list = hand_built(vec![
        one_source(1, Transition::Cut, 0, (HOUR, HOUR + 240)),
        one_source(2, Transition::Dissolve(240), 200, (HOUR + 240, HOUR + 300)),
    ]);
    assert_eq!(
        conform::import(&mut project, &list, RATE, start()),
        Err(IoStatus::ConformDissolveTooLong),
        "an event whose whole length is consumed by its own opening names no frames"
    );
}

#[test]
fn a_record_start_counting_at_another_rate_is_refused() {
    let (project, sequence, _, _) = fixture();
    let pal = Timecode::from_frame_number(HOUR, 25, false).expect("a label");
    assert!(
        conform::export(&project, sequence, pal).is_err(),
        "the record start and the sequence are two statements about one rate"
    );
}

#[test]
fn a_sound_channel_with_a_hole_in_its_numbering_keeps_the_number() {
    // `A3` alone makes three sound tracks, the first two empty. The number is
    // the channel's meaning; closing the hole would silently move a track to a
    // lane nobody put it on.
    let (mut project, _, digest) = library();
    let reel = digest.to_string()[..8].to_owned();
    let event = edl::event(
        1,
        &reel,
        Channel::Audio(3),
        Transition::Cut,
        (at(0), at(48)),
        (at(HOUR), at(HOUR + 48)),
    )
    .expect("an event");
    let list = hand_built(vec![edl::named(event, &digest.to_string())]);
    let landed = conform::import(&mut project, &list, RATE, start()).expect("a sequence");
    let sequence = project.sequence(landed).expect("the sequence");
    assert_eq!(sequence.track_count(), 3);
    assert!(sequence.track(0).expect("a track").is_empty());
    assert!(sequence.track(1).expect("a track").is_empty());
    assert_eq!(sequence.track(2).expect("a track").len(), 1);
}

#[test]
fn conforming_is_not_editing() {
    let (project, sequence, _, _) = fixture();
    let (mut into, landed, _) = round_trip(&project, sequence);
    assert_eq!(
        into.undo(landed),
        Err(media_editor_model::ModelStatus::NothingToDo),
        "offering to undo the import would offer to undo the file"
    );
}

/// The same cut, at two rates that write themselves identically.
fn one_cut(rate: Timebase) -> (Project, SequenceId) {
    let mut project = Project::new();
    let media = project
        .add_media(
            MediaAsset::new(
                Digest::of(b"same"),
                rate,
                Duration::new(9_000, rate).expect("a length"),
            )
            .expect("an asset"),
        )
        .expect("room");
    let sequence = project.add_sequence(rate).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    for (index, source, length) in [(0, 100, 48), (1, 500, 72)] {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index,
                    item: Item::Clip(
                        Clip::new(
                            media,
                            source,
                            Duration::new(length, rate).expect("a length"),
                        )
                        .expect("a clip"),
                    ),
                },
            )
            .expect("a clip");
    }
    project.forget_history();
    (project, sequence)
}

#[test]
fn a_list_read_at_the_wrong_rate_is_caught_by_the_media_and_not_by_the_file() {
    // These bytes are a valid film cut and a valid PAL cut and nothing in them
    // chooses -- that is the format's deepest limitation, and no reader can
    // fix it. What can be said is where the mistake is caught: not by the
    // file, which has no objection, and not by the timecodes, every one of
    // which counts at twenty-five as happily as at twenty-four, but by the
    // media library, which knows what rate the source it names runs at.
    let (project, sequence) = one_cut(Timebase::FILM_24);
    let hour = Timecode::new(1, 0, 0, 0, 24, false).expect("a label");
    let text = edl::write(
        conform::export(&project, sequence, hour)
            .expect("a list")
            .list(),
    )
    .expect("a written list");
    let read = edl::parse(&text).expect("a list that parses");

    let mut into = Project::new();
    for (_, asset) in project.media().iter() {
        into.add_media(asset.clone()).expect("room");
    }
    assert_eq!(
        conform::import(
            &mut into,
            &read,
            Timebase::PAL_25,
            Timecode::new(1, 0, 0, 0, 25, false).expect("a label")
        ),
        Err(IoStatus::Model(
            media_editor_model::ModelStatus::MediaTimebaseMismatch
        )),
        "a twenty-five frame cut cannot be built out of twenty-four frame media"
    );
}

#[test]
fn a_pal_cut_comes_back_at_twenty_five() {
    let (project, sequence) = one_cut(Timebase::PAL_25);
    let origin = Timecode::new(1, 0, 0, 0, 25, false).expect("a label");
    let conformed = conform::export(&project, sequence, origin).expect("a list");
    let text = edl::write(conformed.list()).expect("a written list");
    let read = edl::parse(&text).expect("a list that parses");
    let mut into = Project::new();
    for (_, asset) in project.media().iter() {
        into.add_media(asset.clone()).expect("room");
    }
    let landed = conform::import(&mut into, &read, Timebase::PAL_25, origin).expect("a sequence");
    assert_eq!(
        into.sequence(landed).expect("the sequence"),
        project.sequence(sequence).expect("the sequence"),
        "the rate was told, so the cut is the cut and not one counted at thirty"
    );
}

#[test]
fn a_label_that_cannot_count_at_the_told_rate_is_refused() {
    // Twenty-seven frames is a position no twenty-four frame count reaches. A
    // list said to be a film cut and holding that label is not one, and the
    // honest answer is to say so rather than to take the number.
    let (mut project, _, digest) = library();
    let reel = digest.to_string()[..8].to_owned();
    let late = Timecode::new(1, 0, 0, 27, 30, false).expect("a label at thirty");
    let event = edl::event(
        1,
        &reel,
        Channel::Video,
        Transition::Cut,
        (
            Timecode::new(0, 0, 0, 0, 30, false).expect("a label"),
            Timecode::new(0, 0, 2, 0, 30, false).expect("a label"),
        ),
        (
            late,
            Timecode::new(1, 0, 2, 27, 30, false).expect("a label"),
        ),
    )
    .expect("an event");
    let list = hand_built(vec![edl::named(event, &digest.to_string())]);
    assert_eq!(
        conform::import(&mut project, &list, RATE, start()),
        Err(IoStatus::Time(
            media_editor_core::CoreStatus::TimecodeMalformed
        ))
    );
}

#[test]
fn a_rate_past_thirty_writes_a_list_this_parser_cannot_read_back() {
    // CMX omits the rate, so the parser's 30 fps label cannot represent frame
    // fields 30 through 49 from a 50 fps export.
    let (project, sequence) = one_cut(Timebase::PAL_50);
    let origin = Timecode::new(1, 0, 0, 0, 50, false).expect("a label");
    let conformed = conform::export(&project, sequence, origin).expect("a list");
    let text = edl::write(conformed.list()).expect("a written list");
    assert!(
        text.contains("01:00:00:48"),
        "forty-eight frames at fifty is not yet a second"
    );
    assert_eq!(
        edl::parse(&text),
        Err(IoStatus::Time(
            media_editor_core::CoreStatus::TimecodeMalformed
        ))
    );
}

#[test]
fn a_wipe_is_left_behind_and_comes_back_a_dissolve() {
    // The format has a wipe event, and it names the shape by a *pattern
    // number* -- a convention of whichever machine wrote the list rather than
    // anything the format defines. Inventing one would write a file naming a
    // shape this application refuses to read back, so a wipe is written as the
    // dissolve it is timed like: every frame lands where it belongs and the
    // edge is gone. Which is the definition of a decoration rather than a
    // refusal, by this module's own line.
    let (mut project, sequence, _, _) = fixture();
    let held = project
        .sequence(sequence)
        .expect("the sequence")
        .track(0)
        .expect("the picture track")
        .transitions()[0];
    project
        .apply(
            sequence,
            Edit::RemoveTransition {
                track: 0,
                boundary: held.boundary(),
            },
        )
        .expect("a dissolve off");
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: track::Transition::wiping(
                    held.boundary(),
                    held.duration(),
                    media_editor_model::Wipe::RIGHTWARD,
                )
                .expect("a wipe"),
            },
        )
        .expect("a wipe");

    let conformed = conform::export(&project, sequence, start()).expect("a list");
    assert_eq!(conformed.left_behind().wipes(), 1);
    assert!(!conformed.left_behind().is_empty());
    // The timings are untouched: it is still a D event of the same length in
    // the same place, so the cut is right and only the edge is missing.
    assert_eq!(
        conformed.list().events()[2].transition(),
        Transition::Dissolve(24)
    );

    let (into, landed, _) = round_trip(&project, sequence);
    assert_eq!(
        into.sequence(landed)
            .expect("the sequence")
            .track(0)
            .expect("a track")
            .transitions()[0]
            .kind(),
        media_editor_model::TransitionKind::Dissolve,
        "a reported loss that changed nothing would be a warning nobody should heed"
    );
    assert_ne!(
        into.sequence(landed).expect("the sequence"),
        project.sequence(sequence).expect("the sequence")
    );
}

#[test]
fn one_asset_per_digest_is_what_makes_the_theorem_true() {
    // This is the case that falsified it. Two identifiers naming one digest,
    // a clip on each: the export reports nothing left behind, and the import
    // resolves both clips to whichever identifier it finds first -- so the
    // sequence comes back pointing at one of them and the theorem is a lie.
    //
    // It cannot happen now, because the library holds one asset per digest and
    // adding the same content twice gives back the identifier it already had.
    // So the two clips are the same clip, and the round trip is exact.
    let mut project = Project::new();
    let asset = MediaAsset::new(Digest::of(b"twice"), RATE, frames(9_000)).expect("an asset");
    let one = project.add_media(asset.clone()).expect("room");
    let two = project.add_media(asset).expect("room");
    assert_eq!(one, two, "the library refuses to hold it twice");

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
    for (index, media) in [(0_usize, one), (1, two)] {
        insert(
            &mut project,
            sequence,
            0,
            index,
            clip(
                media,
                i64::try_from(index).expect("a small count") * 100,
                48,
            ),
        );
    }
    project.forget_history();

    let (into, landed, left) = round_trip(&project, sequence);
    assert!(left.is_empty());
    assert_eq!(
        into.sequence(landed).expect("the sequence"),
        project.sequence(sequence).expect("the sequence")
    );
}
