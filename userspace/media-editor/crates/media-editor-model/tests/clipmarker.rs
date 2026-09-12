// SPDX-License-Identifier: GPL-3.0-only
//! A note that travels with the shot.
//!
//! The other half of the pair M8.28 opened, and the whole difference is what
//! the instant is measured from. A sequence's marker names a position in the
//! *programme* and stays where it was put; a clip's names a position in the
//! *shot*, so it moves when the shot moves, survives a trim, goes into the
//! bin with a lift, and is divided by a cut.
//!
//! Which one an editor wants is decided by what the note is about. "The sync
//! drifts here" is about the programme. "His eyeline goes off here" is about
//! the shot, and it should still be on the shot after somebody moves it a
//! minute later.

use media_editor_core::{Digest, Duration, Instant, Timebase};
use media_editor_model::{
    Clip, Edit, Item, MAX_MARKERS_PER_CLIP, Marker, MediaAsset, MediaId, ModelStatus, Project,
    SequenceId, TrackKind,
};

const RATE: Timebase = Timebase::FILM_24;
const IN_POINT: i64 = 100;
const LENGTH: i64 = 48;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a length")
}

fn at(ticks: i64) -> Instant {
    Instant::new(ticks, RATE)
}

fn note(ticks: i64, text: &str) -> Marker {
    Marker::new(at(ticks), text.into()).expect("a note")
}

fn media_id() -> MediaId {
    let mut project = Project::new();
    project
        .add_media(MediaAsset::new(Digest::of(b"footage"), RATE, frames(9_000)).expect("an asset"))
        .expect("room")
}

fn clip() -> Clip {
    Clip::new(media_id(), IN_POINT, frames(LENGTH)).expect("a clip")
}

/// The offsets a clip's notes sit at, in the order it holds them.
fn offsets(held: &Clip) -> Vec<i64> {
    held.markers()
        .iter()
        .map(|note| note.at().ticks())
        .collect()
}

#[test]
fn a_note_goes_on_a_shot_at_an_offset_from_its_start() {
    let noted = clip()
        .with_marker(note(12, "eyeline"))
        .expect("a note")
        .with_marker(note(30, "boom in shot"))
        .expect("a note");
    assert_eq!(offsets(&noted), vec![12, 30]);
    assert_eq!(
        noted.marker_at(at(12)).expect("a note").text(),
        "eyeline",
        "the note at twelve is not the one that was put there"
    );
    assert_eq!(noted.marker_at(at(13)), None);
}

#[test]
fn notes_are_kept_in_time_order() {
    // Put on backwards, held forwards. A list of notes that is not the
    // timeline is a list somebody has to sort before showing.
    let noted = clip()
        .with_marker(note(30, "third"))
        .expect("a note")
        .with_marker(note(4, "first"))
        .expect("a note")
        .with_marker(note(12, "second"))
        .expect("a note");
    assert_eq!(offsets(&noted), vec![4, 12, 30]);
}

#[test]
fn two_notes_at_one_offset_are_refused() {
    let noted = clip().with_marker(note(12, "eyeline")).expect("a note");
    assert_eq!(
        noted.with_marker(note(12, "or is it")),
        Err(ModelStatus::MarkerExists)
    );
}

#[test]
fn a_note_before_the_shot_starts_is_refused() {
    assert_eq!(
        clip().with_marker(note(-1, "before")),
        Err(ModelStatus::MarkerBeforeStart)
    );
}

#[test]
fn a_note_counted_another_way_is_refused() {
    let elsewhere =
        Marker::new(Instant::new(4, Timebase::NTSC_30), "elsewhere".into()).expect("a note");
    assert_eq!(
        clip().with_marker(elsewhere),
        Err(ModelStatus::WrongTimebase)
    );
}

#[test]
fn a_shot_holds_no_more_notes_than_the_bound() {
    let mut noted = clip();
    for index in 0..MAX_MARKERS_PER_CLIP {
        noted = noted
            .with_marker(note(i64::try_from(index).expect("a tick"), ""))
            .expect("a note");
    }
    assert_eq!(noted.markers().len(), MAX_MARKERS_PER_CLIP);
    assert_eq!(
        noted.with_marker(note(1_000, "one too many")),
        Err(ModelStatus::CapacityExhausted)
    );
}

#[test]
fn a_note_past_the_end_is_carried_rather_than_dropped() {
    // A trim is not a delete. Shorten a shot and the notes on the part that
    // is now hidden are still there when somebody pulls it back out -- which
    // is what a curve on a clip already does, held past its end rather than
    // refused for reaching past it.
    let noted = clip().with_marker(note(40, "late")).expect("a note");
    let trimmed = noted.with_duration(frames(20)).expect("a trim");
    assert_eq!(offsets(&trimmed), vec![40], "the trim deleted the note");
    let back = trimmed.with_duration(frames(LENGTH)).expect("a trim");
    assert_eq!(offsets(&back), vec![40]);
    assert_eq!(back, noted, "the round trip is not the clip that went in");
}

#[test]
fn a_note_comes_off_and_says_what_it_said() {
    let noted = clip()
        .with_marker(note(12, "eyeline"))
        .expect("a note")
        .with_marker(note(30, "boom"))
        .expect("a note");
    let (bare, taken) = noted.without_marker(at(12)).expect("a removal");
    assert_eq!(taken.text(), "eyeline");
    assert_eq!(offsets(&bare), vec![30]);
    assert_eq!(
        bare.without_marker(at(12)),
        Err(ModelStatus::NoSuchMarker),
        "it came off twice"
    );
}

#[test]
fn a_cut_divides_the_notes_and_a_join_puts_them_back() {
    // A note at the cut goes to the tail. A note at tick k names the frame at
    // k, the head's frames are the ones below k, and that frame is now the
    // tail's first -- the same half-open convention every span in this model
    // uses.
    let whole = Item::Clip(
        clip()
            .with_marker(note(4, "head"))
            .expect("a note")
            .with_marker(note(20, "at the cut"))
            .expect("a note")
            .with_marker(note(35, "tail"))
            .expect("a note"),
    );
    let (head, tail) = whole.split(20).expect("a cut");
    let Item::Clip(head_clip) = &head else {
        panic!("a clip");
    };
    let Item::Clip(tail_clip) = &tail else {
        panic!("a clip");
    };
    assert_eq!(offsets(head_clip), vec![4]);
    assert_eq!(
        offsets(tail_clip),
        vec![0, 15],
        "the note at the cut belongs to the tail, at its own nought"
    );
    assert_eq!(
        tail_clip.marker_at(at(0)).expect("a note").text(),
        "at the cut"
    );
    assert_eq!(head.join(&tail).expect("a join"), whole);
}

#[test]
fn a_cut_anywhere_is_undone_by_a_join() {
    let whole = Item::Clip(
        clip()
            .with_marker(note(0, "top"))
            .expect("a note")
            .with_marker(note(4, "early"))
            .expect("a note")
            .with_marker(note(47, "last frame"))
            .expect("a note"),
    );
    for cut in 1..LENGTH {
        let (head, tail) = whole.split(cut).expect("a cut");
        assert_eq!(
            head.join(&tail).expect("a join"),
            whole,
            "a cut at {cut} does not join back"
        );
    }
}

#[test]
fn a_join_that_would_overflow_the_bound_is_refused() {
    // Two shots, adjacent in their media, each carrying the bound's worth of
    // notes. Joining them would make one shot holding twice the bound.
    let mut first = clip();
    let mut second = Clip::new(media_id(), IN_POINT + LENGTH, frames(LENGTH)).expect("a clip");
    for index in 0..MAX_MARKERS_PER_CLIP {
        let tick = i64::try_from(index).expect("a tick");
        first = first.with_marker(note(tick, "")).expect("a note");
        second = second.with_marker(note(tick, "")).expect("a note");
    }
    assert_eq!(
        Item::Clip(first).join(&Item::Clip(second)),
        Err(ModelStatus::CapacityExhausted)
    );
}

/// A project with one clip on a picture track.
fn project() -> (Project, SequenceId) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("room");
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"footage"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
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
                item: Item::Clip(Clip::new(media, IN_POINT, frames(LENGTH)).expect("a clip")),
            },
        )
        .expect("a clip");
    project.forget_history();
    (project, sequence)
}

fn held(project: &Project, sequence: SequenceId) -> Clip {
    let Item::Clip(clip) = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item")
    else {
        panic!("a clip");
    };
    clip.clone()
}

#[test]
fn the_edit_puts_a_note_on_and_undo_takes_it_off() {
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::AddClipMarker {
                track: 0,
                index: 0,
                at: at(12),
                text: "eyeline".into(),
            },
        )
        .expect("a note");
    assert_eq!(offsets(&held(&project, sequence)), vec![12]);
    project.undo(sequence).expect("an undo");
    assert!(held(&project, sequence).markers().is_empty());
    project.redo(sequence).expect("a redo");
    assert_eq!(
        held(&project, sequence)
            .marker_at(at(12))
            .expect("a note")
            .text(),
        "eyeline"
    );
}

#[test]
fn undoing_a_removal_puts_the_text_back() {
    // Nothing else remembers what the note said once the clip no longer has
    // it, so the inverse has to carry it.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::AddClipMarker {
                track: 0,
                index: 0,
                at: at(12),
                text: "boom in shot".into(),
            },
        )
        .expect("a note");
    project.forget_history();
    project
        .apply(
            sequence,
            Edit::RemoveClipMarker {
                track: 0,
                index: 0,
                at: at(12),
            },
        )
        .expect("a removal");
    assert!(held(&project, sequence).markers().is_empty());
    project.undo(sequence).expect("an undo");
    assert_eq!(
        held(&project, sequence)
            .marker_at(at(12))
            .expect("a note")
            .text(),
        "boom in shot"
    );
}

#[test]
fn a_move_is_one_edit_and_its_own_inverse() {
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::AddClipMarker {
                track: 0,
                index: 0,
                at: at(12),
                text: "eyeline".into(),
            },
        )
        .expect("a note");
    project.forget_history();
    project
        .apply(
            sequence,
            Edit::MoveClipMarker {
                track: 0,
                index: 0,
                from: at(12),
                to: at(30),
            },
        )
        .expect("a move");
    assert_eq!(offsets(&held(&project, sequence)), vec![30]);
    project.undo(sequence).expect("an undo");
    assert_eq!(offsets(&held(&project, sequence)), vec![12]);
    assert_eq!(
        held(&project, sequence)
            .marker_at(at(12))
            .expect("a note")
            .text(),
        "eyeline",
        "the move put the text through the history and lost it"
    );
}

#[test]
fn a_refused_move_puts_the_note_back() {
    let (mut project, sequence) = project();
    for (tick, text) in [(12_i64, "eyeline"), (30, "boom")] {
        project
            .apply(
                sequence,
                Edit::AddClipMarker {
                    track: 0,
                    index: 0,
                    at: at(tick),
                    text: text.into(),
                },
            )
            .expect("a note");
    }
    assert_eq!(
        project.apply(
            sequence,
            Edit::MoveClipMarker {
                track: 0,
                index: 0,
                from: at(12),
                to: at(30),
            },
        ),
        Err(ModelStatus::MarkerExists)
    );
    assert_eq!(
        offsets(&held(&project, sequence)),
        vec![12, 30],
        "a move that did not happen deleted the note it was moving"
    );
}

#[test]
fn a_gap_has_nothing_to_leave_a_note_on() {
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 1,
                item: Item::gap(frames(10)).expect("a gap"),
            },
        )
        .expect("a gap");
    assert_eq!(
        project.apply(
            sequence,
            Edit::AddClipMarker {
                track: 0,
                index: 1,
                at: at(2),
                text: "on nothing".into(),
            },
        ),
        Err(ModelStatus::NotAClip)
    );
}

#[test]
fn a_note_travels_with_the_shot_through_a_lift() {
    // The property that makes this a different feature from a sequence's
    // markers rather than a re-spelling of them: the note is on the shot, so
    // it goes wherever the shot goes.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::AddClipMarker {
                track: 0,
                index: 0,
                at: at(12),
                text: "eyeline".into(),
            },
        )
        .expect("a note");
    project
        .apply(sequence, Edit::LiftItem { track: 0, index: 0 })
        .expect("a lift");
    // The gap the lift left carries nothing, and the drop that undoes it puts
    // the shot back with its note on.
    project.undo(sequence).expect("an undo");
    assert_eq!(
        held(&project, sequence)
            .marker_at(at(12))
            .expect("a note")
            .text(),
        "eyeline",
        "the lift dropped the note off the shot"
    );
}
