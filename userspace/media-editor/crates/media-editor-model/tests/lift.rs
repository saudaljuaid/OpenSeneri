// SPDX-License-Identifier: GPL-3.0-only
//! Taking a shot off a track without moving what follows it.
//!
//! The model has had [`Edit::RemoveItem`] since it had items, and that edit
//! moves everything after the hole earlier — which an editor calls a ripple
//! delete, or an extract. This is the other one: the shot goes and the hole
//! stays.
//!
//! Which of the two somebody wants is not a preference, it is a question about
//! the **rest of the programme**. Sound cut to picture stays in sync through a
//! lift and slides through an extract; a title two minutes later stays where it
//! was written through a lift and arrives early through an extract. An editor
//! offering only one is an editor making that choice on the user's behalf and
//! not telling them.

use media_editor_core::{Digest, Duration, Instant, Timebase};
use media_editor_model::{
    Clip, Edit, Item, MediaAsset, ModelStatus, Project, SequenceId, TrackKind, Transition,
};

const RATE: Timebase = Timebase::FILM_24;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a length")
}

fn at(tick: i64) -> Instant {
    Instant::new(tick, RATE)
}

/// One picture track of three fifty-frame shots.
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
    for index in 0..3_usize {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index,
                    item: Item::Clip(
                        Clip::new(
                            media,
                            1_000 * i64::try_from(index).expect("a tick"),
                            frames(50),
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

fn item(project: &Project, sequence: SequenceId, index: usize) -> Item {
    project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(index)
        .expect("an item")
        .clone()
}

#[test]
fn a_lift_leaves_a_hole_and_moves_nothing() {
    // The property that separates it from a ripple delete, and it is asserted
    // on the *third* shot's position rather than only on the second slot --
    // a fixture that checked the hole and not what follows it could not tell
    // the two edits apart.
    let (mut project, sequence) = project();
    let third = item(&project, sequence, 2);
    let length = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .duration();

    project
        .apply(sequence, Edit::LiftItem { track: 0, index: 1 })
        .expect("a lift");

    assert_eq!(
        item(&project, sequence, 1),
        Item::gap(frames(50)).expect("a gap"),
        "the shot did not become a hole as long as itself"
    );
    assert_eq!(
        item(&project, sequence, 2),
        third,
        "the shot after the hole is not the shot that was there"
    );
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .item_start(2)
            .expect("a start"),
        at(100),
        "the shot after the hole moved, which is an extract and not a lift"
    );
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .duration(),
        length,
        "the programme changed length"
    );
}

#[test]
fn an_extract_is_the_other_edit_and_still_is() {
    // The same index produces different programmes for lift and extract.
    let (mut lifted, sequence) = project();
    let mut extracted = lifted.clone();

    lifted
        .apply(sequence, Edit::LiftItem { track: 0, index: 1 })
        .expect("a lift");
    extracted
        .apply(sequence, Edit::RemoveItem { track: 0, index: 1 })
        .expect("an extract");

    assert_eq!(
        lifted
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .items()
            .len(),
        3,
    );
    assert_eq!(
        extracted
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .items()
            .len(),
        2,
    );
    assert_ne!(
        lifted.sequence(sequence).expect("a sequence"),
        extracted.sequence(sequence).expect("a sequence"),
        "a lift and an extract produced the same programme, so one of them is \
         not doing what its name says"
    );
}

#[test]
fn the_inverse_puts_the_shot_back_exactly() {
    let (mut project, sequence) = project();
    let before = project.clone();
    let shot = item(&project, sequence, 1);

    project
        .apply(sequence, Edit::LiftItem { track: 0, index: 1 })
        .expect("a lift");
    project.undo(sequence).expect("undone");

    assert_eq!(item(&project, sequence, 1), shot);
    assert_eq!(
        project.sequence(sequence).expect("a sequence"),
        before.sequence(sequence).expect("a sequence"),
        "undoing a lift did not reproduce the sequence exactly"
    );
    project.redo(sequence).expect("redone");
    assert_eq!(
        item(&project, sequence, 1),
        Item::gap(frames(50)).expect("a gap")
    );
}

#[test]
fn a_lifted_shot_keeps_everything_it_carried() {
    // A lift is a change to *where a shot is*, not to what it is. The item
    // travels in the inverse, so anything it loses on the way is lost when
    // somebody presses undo -- which is the worst moment to find out.
    let (mut project, sequence) = project();
    let grade = Digest::of(b"a look");
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 1,
                grade: Some(grade),
            },
        )
        .expect("a grade");
    project
        .apply(
            sequence,
            Edit::SetClipFades {
                track: 0,
                index: 1,
                fade_in: frames(5),
                fade_out: frames(5),
            },
        )
        .expect("fades");
    project.forget_history();

    project
        .apply(sequence, Edit::LiftItem { track: 0, index: 1 })
        .expect("a lift");
    project.undo(sequence).expect("undone");

    let Item::Clip(clip) = item(&project, sequence, 1) else {
        panic!("a clip");
    };
    assert_eq!(clip.grade(), Some(grade), "the shot came back ungraded");
    assert_eq!(clip.fade_in(), frames(5), "the shot came back unfaded");
}

#[test]
fn a_gap_cannot_be_lifted() {
    // Lifting a gap leaves the track exactly as it found it, and an edit that
    // changes nothing still takes a place in the history and still claims, on
    // undo, to have put something back.
    let (mut project, sequence) = project();
    project
        .apply(sequence, Edit::LiftItem { track: 0, index: 1 })
        .expect("a lift");
    assert_eq!(
        project
            .apply(sequence, Edit::LiftItem { track: 0, index: 1 })
            .expect_err("a refusal"),
        ModelStatus::NotAClip
    );
}

#[test]
fn an_item_a_transition_touches_cannot_be_lifted() {
    // A dissolve reaches into the clips on both sides of its cut. Lifting one
    // of them would leave it mixing a gap, which the layer stack refuses at
    // the frame -- and a refusal at the frame is a refusal nobody can act on.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(10)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");

    // The dissolve is at boundary one, between items nought and one, so
    // neither of those may be lifted.
    for index in [0_usize, 1] {
        assert_eq!(
            project
                .apply(sequence, Edit::LiftItem { track: 0, index })
                .expect_err("a refusal"),
            ModelStatus::TransitionWouldLoseItsClip,
            "item {index} touches the dissolve"
        );
    }
    // And the third does not touch it, so it lifts.
    project
        .apply(sequence, Edit::LiftItem { track: 0, index: 2 })
        .expect("a lift past the dissolve");
}

#[test]
fn a_dissolve_later_in_the_programme_does_not_refuse_a_lift_at_its_head() {
    // Lift checks only transitions touching the item. Track::transition_from
    // is intentionally broader for edits such as split that renumber later
    // boundaries.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                // Between items one and two, so item nought touches neither
                // side of it.
                transition: Transition::new(2, frames(10)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");

    project
        .apply(sequence, Edit::LiftItem { track: 0, index: 0 })
        .expect("a lift at the head of a programme with a dissolve at its end");
    assert_eq!(
        item(&project, sequence, 0),
        Item::gap(frames(50)).expect("a gap")
    );
    // And the dissolve is still where it was drawn, on the same two clips.
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .transitions()
            .len(),
        1,
    );
}

#[test]
fn a_shot_goes_back_only_into_the_gap_it_left() {
    // The inverse is not a general "replace this item". A history that could
    // drop a shot into a slot something else had happened to since would be a
    // history describing a project nobody edited.
    let (mut project, sequence) = project();
    let shot = item(&project, sequence, 1);

    // Into a slot that is not a gap at all.
    assert_eq!(
        project
            .apply(
                sequence,
                Edit::DropItem {
                    track: 0,
                    index: 2,
                    item: shot.clone(),
                },
            )
            .expect_err("a refusal"),
        ModelStatus::NotTheGapThatWasLifted
    );

    // And into a gap of the wrong length.
    project
        .apply(sequence, Edit::LiftItem { track: 0, index: 1 })
        .expect("a lift");
    project
        .apply(
            sequence,
            Edit::SetItemDuration {
                track: 0,
                index: 1,
                duration: frames(20),
            },
        )
        .expect("a trim");
    assert_eq!(
        project
            .apply(
                sequence,
                Edit::DropItem {
                    track: 0,
                    index: 1,
                    item: shot,
                },
            )
            .expect_err("a refusal"),
        ModelStatus::NotTheGapThatWasLifted
    );
}

#[test]
fn a_refused_lift_writes_nothing() {
    // R-1.4. The transition check runs before the gap is built and before the
    // item is replaced, so the track is exactly as it was.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(10)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");
    project.forget_history();
    let before = project.clone();

    assert!(
        project
            .apply(sequence, Edit::LiftItem { track: 0, index: 0 })
            .is_err()
    );
    assert_eq!(
        project.sequence(sequence).expect("a sequence"),
        before.sequence(sequence).expect("a sequence"),
    );
    assert_eq!(
        project.undo(sequence).expect_err("a refusal"),
        ModelStatus::NothingToDo,
        "a refused edit took a place in the history"
    );
}

#[test]
fn a_lift_and_a_razor_compose() {
    // The gesture this exists for: blade a shot in two, then take the half you
    // do not want without moving anything after it.
    let (mut project, sequence) = project();
    let named = project
        .sequence(sequence)
        .expect("a sequence")
        .cuttable_at(at(25))
        .expect("a set");
    project
        .apply(
            sequence,
            Edit::CutAt {
                at: at(25),
                tracks: named,
            },
        )
        .expect("a razor");
    project
        .apply(sequence, Edit::LiftItem { track: 0, index: 0 })
        .expect("a lift");

    assert_eq!(
        item(&project, sequence, 0),
        Item::gap(frames(25)).expect("a gap")
    );
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .item_start(2)
            .expect("a start"),
        at(50),
        "the shots after the lift moved"
    );
    // Two undos put both back, in order, because each is one edit.
    project.undo(sequence).expect("undone");
    project.undo(sequence).expect("undone");
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .items()
            .len(),
        3,
    );
}
