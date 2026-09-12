// SPDX-License-Identifier: GPL-3.0-only
//! Rolling a cut and sliding an item: the two trims a track could not do.
//!
//! Both are defined by what they *do not* change. A roll moves a cut without
//! changing how long the programme is, so nothing after the cut moves and
//! nothing after the cut has to be moved back. A slide moves an item without
//! changing the item, so the shot that was chosen is still the shot that is
//! there — only later.
//!
//! Both are their own inverses with the sign turned round, which is why
//! neither edit has to remember what it replaced.
//!
//! And one thing that was already true and worth pinning: because a track
//! stores no positions, **removing an item is a ripple delete** — there is no
//! second operation that closes the hole, because there is never a hole.

use media_editor_core::{Digest, Duration, Timebase};
use media_editor_model::{
    Clip, Edit, Item, MediaAsset, ModelStatus, Project, SequenceId, TrackKind, Transition,
};

const RATE: Timebase = Timebase::FILM_24;

/// How long each clip in the fixture is.
const LENGTH: i64 = 48;

/// How far into its media each clip starts.
///
/// Not nought, deliberately: a roll that moves a cut earlier needs the
/// incoming clip to have material before its in point, and a fixture whose
/// clips all began at their media's first frame could only ever test the roll
/// that goes one way.
const HANDLE: i64 = 24;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a length")
}

/// A project with one track of three clips, each `LENGTH` long.
fn project() -> (Project, SequenceId) {
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
    for index in 0..3 {
        let mut bytes = [0_u8; 32];
        bytes[0] = u8::try_from(index).expect("a tag");
        let media = project
            .add_media(MediaAsset::new(Digest::new(bytes), RATE, frames(9_000)).expect("an asset"))
            .expect("room");
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index,
                    item: Item::Clip(Clip::new(media, HANDLE, frames(LENGTH)).expect("a clip")),
                },
            )
            .expect("a clip");
    }
    project.forget_history();
    (project, sequence)
}

/// The lengths of a track's items, and where each clip reads from.
fn shape(project: &Project, sequence: SequenceId) -> Vec<(i64, Option<i64>)> {
    project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .items()
        .iter()
        .map(|item| {
            (
                item.duration().ticks(),
                match item {
                    Item::Clip(clip) => Some(clip.source_start()),
                    Item::Gap(_) => None,
                },
            )
        })
        .collect()
}

fn total(project: &Project, sequence: SequenceId) -> i64 {
    project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .duration()
        .expect("a length")
        .ticks()
}

#[test]
fn a_roll_moves_the_cut_and_not_the_programme() {
    let (mut project, sequence) = project();
    let before = total(&project, sequence);
    project
        .apply(
            sequence,
            Edit::RollCut {
                track: 0,
                boundary: 1,
                by: 12,
            },
        )
        .expect("a roll");
    assert_eq!(
        shape(&project, sequence),
        vec![
            (LENGTH + 12, Some(HANDLE)),
            (LENGTH - 12, Some(HANDLE + 12)),
            (LENGTH, Some(HANDLE)),
        ],
        "the outgoing clip runs on and the incoming one starts later in itself"
    );
    assert_eq!(
        total(&project, sequence),
        before,
        "and the programme is exactly as long as it was"
    );
}

#[test]
fn a_roll_can_go_either_way() {
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::RollCut {
                track: 0,
                boundary: 2,
                by: -20,
            },
        )
        .expect("a roll");
    assert_eq!(
        shape(&project, sequence),
        vec![
            (LENGTH, Some(HANDLE)),
            (LENGTH - 20, Some(HANDLE)),
            (LENGTH + 20, Some(HANDLE - 20)),
        ],
        "the cut moved earlier, so the incoming clip reaches back into its handle"
    );
}

#[test]
fn a_roll_is_its_own_inverse() {
    let (mut project, sequence) = project();
    let original = project.sequence(sequence).expect("a sequence").clone();
    for by in [7, -7] {
        project
            .apply(
                sequence,
                Edit::RollCut {
                    track: 0,
                    boundary: 1,
                    by,
                },
            )
            .expect("a roll");
    }
    assert_eq!(
        project.sequence(sequence).expect("a sequence"),
        &original,
        "equal by the whole value, so a field nobody thought to compare is"
    );
}

#[test]
fn undoing_a_roll_restores_the_sequence() {
    // The same property through the journal rather than by rolling back, so
    // the inverse the edit hands out is the one under test rather than the
    // one this file computed.
    let (mut project, sequence) = project();
    let original = project.sequence(sequence).expect("a sequence").clone();
    project
        .apply(
            sequence,
            Edit::RollCut {
                track: 0,
                boundary: 1,
                by: 13,
            },
        )
        .expect("a roll");
    assert_ne!(project.sequence(sequence).expect("a sequence"), &original);
    project.undo(sequence).expect("an undo");
    assert_eq!(project.sequence(sequence).expect("a sequence"), &original);
}

#[test]
fn undoing_a_slide_restores_the_sequence() {
    // Through the journal rather than by sliding back, so the inverse the edit
    // hands out is what is under test. Applying `+by` then `-by` by hand does
    // not exercise the inverse at all — it exercises this file's arithmetic —
    // and a control that made the inverse `by` itself passed that way.
    let (mut project, sequence) = project();
    let original = project.sequence(sequence).expect("a sequence").clone();
    project
        .apply(
            sequence,
            Edit::SlideItem {
                track: 0,
                index: 1,
                by: 11,
            },
        )
        .expect("a slide");
    assert_ne!(project.sequence(sequence).expect("a sequence"), &original);
    project.undo(sequence).expect("an undo");
    assert_eq!(project.sequence(sequence).expect("a sequence"), &original);
}

#[test]
fn a_roll_that_would_eat_a_clip_is_refused() {
    let (mut project, sequence) = project();
    let before = shape(&project, sequence);
    for by in [LENGTH, -LENGTH] {
        assert_eq!(
            project.apply(
                sequence,
                Edit::RollCut {
                    track: 0,
                    boundary: 1,
                    by,
                },
            ),
            Err(ModelStatus::EmptyItem),
            "a roll of {by} consumes a whole side"
        );
    }
    assert_eq!(
        shape(&project, sequence),
        before,
        "and neither refusal moved anything (R-1.4)"
    );
}

#[test]
fn a_roll_before_the_start_of_the_media_is_refused() {
    // The incoming clip has `HANDLE` ticks of material before its in point and
    // no more. Asking for one more than that is asking for a frame that was
    // never shot.
    let (mut reaching, sequence) = project();
    assert!(
        reaching
            .apply(
                sequence,
                Edit::RollCut {
                    track: 0,
                    boundary: 1,
                    by: -HANDLE,
                },
            )
            .is_ok(),
        "exactly the handle is reachable"
    );
    let (mut project, sequence) = project();
    assert_eq!(
        project.apply(
            sequence,
            Edit::RollCut {
                track: 0,
                boundary: 1,
                by: -HANDLE - 1,
            },
        ),
        Err(ModelStatus::SourceBeforeStart)
    );
}

#[test]
fn a_cut_that_is_not_between_two_items_cannot_roll() {
    let (mut project, sequence) = project();
    for boundary in [0, 3, 40] {
        assert_eq!(
            project.apply(
                sequence,
                Edit::RollCut {
                    track: 0,
                    boundary,
                    by: 1,
                },
            ),
            Err(ModelStatus::UnknownItem),
            "boundary {boundary}"
        );
    }
}

#[test]
fn a_slide_moves_an_item_and_leaves_it_untouched() {
    let (mut project, sequence) = project();
    let before = total(&project, sequence);
    let middle = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(1)
        .expect("an item")
        .clone();
    project
        .apply(
            sequence,
            Edit::SlideItem {
                track: 0,
                index: 1,
                by: 9,
            },
        )
        .expect("a slide");
    assert_eq!(
        shape(&project, sequence),
        vec![
            (LENGTH + 9, Some(HANDLE)),
            (LENGTH, Some(HANDLE)),
            (LENGTH - 9, Some(HANDLE + 9)),
        ]
    );
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .item(1)
            .expect("an item"),
        &middle,
        "the item that slid is the item that was there, unchanged"
    );
    assert_eq!(total(&project, sequence), before);
}

#[test]
fn a_slide_is_its_own_inverse() {
    let (mut project, sequence) = project();
    let original = project.sequence(sequence).expect("a sequence").clone();
    for by in [-11, 11] {
        project
            .apply(
                sequence,
                Edit::SlideItem {
                    track: 0,
                    index: 1,
                    by,
                },
            )
            .expect("a slide");
    }
    assert_eq!(project.sequence(sequence).expect("a sequence"), &original);
}

#[test]
fn an_item_at_either_end_of_a_track_cannot_slide() {
    // Nothing to take the difference. Inventing a gap to absorb it would be a
    // different edit performed silently.
    let (mut project, sequence) = project();
    for index in [0, 2] {
        assert_eq!(
            project.apply(
                sequence,
                Edit::SlideItem {
                    track: 0,
                    index,
                    by: 1,
                },
            ),
            Err(ModelStatus::UnknownItem),
            "item {index}"
        );
    }
}

#[test]
fn a_slide_that_would_eat_a_neighbour_is_refused() {
    let (mut project, sequence) = project();
    let before = shape(&project, sequence);
    assert_eq!(
        project.apply(
            sequence,
            Edit::SlideItem {
                track: 0,
                index: 1,
                by: LENGTH,
            },
        ),
        Err(ModelStatus::EmptyItem)
    );
    assert_eq!(shape(&project, sequence), before);
}

#[test]
fn a_gap_rolls_like_anything_else() {
    // A gap has no source to move, so only its length changes -- and a roll
    // that shortens a gap from the front is the one place the source arithmetic
    // must be skipped rather than applied to nothing.
    let (mut project, sequence) = project();
    project
        .apply(sequence, Edit::RemoveItem { track: 0, index: 1 })
        .expect("a removal");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 1,
                item: Item::gap(frames(LENGTH)).expect("a gap"),
            },
        )
        .expect("a gap");
    project
        .apply(
            sequence,
            Edit::RollCut {
                track: 0,
                boundary: 1,
                by: 10,
            },
        )
        .expect("a roll");
    assert_eq!(
        shape(&project, sequence),
        vec![
            (LENGTH + 10, Some(HANDLE)),
            (LENGTH - 10, None),
            (LENGTH, Some(HANDLE)),
        ]
    );
}

#[test]
fn a_roll_that_would_leave_a_dissolve_no_room_is_refused() {
    // A dissolve's two conditions are about exactly what a roll changes: how
    // long each side is, and how far into its media the incoming one starts.
    // A check that only ran when somebody drew the dissolve would let a later
    // trim leave the track describing a transition it cannot perform.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(40)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");
    let before = shape(&project, sequence);
    assert_eq!(
        project.apply(
            sequence,
            Edit::RollCut {
                track: 0,
                boundary: 1,
                by: 20,
            },
        ),
        Err(ModelStatus::TransitionTooLong),
        "the incoming clip would be shorter than the dissolve across it"
    );
    assert_eq!(
        shape(&project, sequence),
        before,
        "and the refusal left the track alone"
    );
    // A roll the dissolve still fits is allowed, which is what keeps the
    // check a check rather than a ban.
    project
        .apply(
            sequence,
            Edit::RollCut {
                track: 0,
                boundary: 1,
                by: 4,
            },
        )
        .expect("a roll");
    assert_eq!(shape(&project, sequence)[0].0, LENGTH + 4);
}

#[test]
fn a_roll_that_would_leave_a_dissolve_no_handle_is_refused() {
    // The other of the two conditions, which the length check cannot stand in
    // for: both clips stay longer than the dissolve, and the incoming one no
    // longer has half of it in front of its in point.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(24)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");
    assert_eq!(
        project.apply(
            sequence,
            Edit::RollCut {
                track: 0,
                boundary: 1,
                by: -14,
            },
        ),
        Err(ModelStatus::SourceBeforeStart)
    );
}

#[test]
fn removing_an_item_is_already_a_ripple_delete() {
    // Worth pinning rather than assuming. A track stores no positions -- an
    // item's place is the sum of the lengths before it -- so there is no hole
    // to close and no second operation that closes it. The property is that
    // everything after the removal is exactly its own length earlier.
    let (mut project, sequence) = project();
    let before = total(&project, sequence);
    let started = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item_start(2)
        .expect("a start")
        .ticks();
    project
        .apply(sequence, Edit::RemoveItem { track: 0, index: 1 })
        .expect("a removal");
    assert_eq!(total(&project, sequence), before - LENGTH);
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .item_start(1)
            .expect("a start")
            .ticks(),
        started - LENGTH,
        "what was third is now second, and it starts a clip's length earlier"
    );
}

#[test]
fn a_roll_of_nothing_changes_nothing() {
    let (mut project, sequence) = project();
    let original = project.sequence(sequence).expect("a sequence").clone();
    project
        .apply(
            sequence,
            Edit::RollCut {
                track: 0,
                boundary: 1,
                by: 0,
            },
        )
        .expect("a roll of nothing");
    assert_eq!(project.sequence(sequence).expect("a sequence"), &original);
}

#[test]
fn a_roll_keeps_everything_a_clip_carries() {
    // The fault found three times over -- a rebuild through `Clip::new` drops
    // what it was not told about -- asked of the two trims, because both of
    // them rebuild a clip from a length and a source position.
    let (mut project, sequence) = project();
    let grade = Some(Digest::of(b"a look"));
    for index in [0, 1] {
        project
            .apply(
                sequence,
                Edit::SetClipGrade {
                    track: 0,
                    index,
                    grade,
                },
            )
            .expect("a grade");
    }
    project
        .apply(
            sequence,
            Edit::RollCut {
                track: 0,
                boundary: 1,
                by: 6,
            },
        )
        .expect("a roll");
    for index in [0, 1] {
        let Item::Clip(clip) = project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .item(index)
            .expect("an item")
        else {
            panic!("a clip");
        };
        assert_eq!(clip.grade(), grade, "item {index} kept its look");
    }
}
