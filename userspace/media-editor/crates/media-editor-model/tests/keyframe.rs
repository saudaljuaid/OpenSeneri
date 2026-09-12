// SPDX-License-Identifier: GPL-3.0-only
//! Editing one keyframe rather than replacing a whole curve.
//!
//! Replacing the curve is correct and coarse: it undoes, but a journal entry
//! carries two whole curves, and fifty drags of one keyframe are fifty copies
//! of everything else on the lane. These edits are what a keyframe *drag* is.
//!
//! Most of what follows is about inverses, because that is where this kind of
//! edit goes wrong. An operation that changes two things at once — adding a
//! keyframe *and* turning a lane on — has to undo both or neither.

use media_editor_core::{Duration, Instant, Rational, Timebase};
use media_editor_model::curve::{Automation, Interpolation, Keyframe, KeyframeEdit};
use media_editor_model::{
    Clip, Edit, Item, MediaAsset, ModelStatus, Project, SequenceId, TrackKind,
};

const RATE: Timebase = Timebase::FILM_24;

fn at(frame: i64) -> Instant {
    Instant::new(frame, RATE)
}

fn value(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a value")
}

fn key(frame: i64, numerator: i64, denominator: i64) -> Keyframe {
    Keyframe::new(
        at(frame),
        value(numerator, denominator),
        Interpolation::Linear,
    )
    .expect("a keyframe")
}

/// A project with one picture track holding one clip, and one sound track.
fn project() -> (Project, SequenceId) {
    let mut project = Project::new();
    let media = project
        .add_media(
            MediaAsset::new(
                media_editor_model::Digest::of(b"one"),
                RATE,
                Duration::new(240, RATE).expect("a duration"),
            )
            .expect("an asset"),
        )
        .expect("media");
    let sequence = project.add_sequence(RATE).expect("a sequence");
    for (index, kind) in [(0, TrackKind::Video), (1, TrackKind::Audio)] {
        project
            .apply(sequence, Edit::AddTrack { index, kind })
            .expect("a track");
    }
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(
                    Clip::new(media, 0, Duration::new(48, RATE).expect("a duration"))
                        .expect("a clip"),
                ),
            },
        )
        .expect("a clip");
    (project, sequence)
}

/// Apply a keyframe operation to the picture track's opacity.
fn opacity(project: &mut Project, sequence: SequenceId, operation: KeyframeEdit) {
    project
        .apply(
            sequence,
            Edit::Keyframe {
                track: 0,
                lane: Automation::Opacity,
                operation,
            },
        )
        .expect("a keyframe edit");
}

/// The picture track's keyframes, as (frame, value) pairs.
fn shape(project: &Project, sequence: SequenceId) -> std::vec::Vec<(i64, Rational)> {
    project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .curve(Automation::Opacity)
        .map(|curve| {
            curve
                .keyframes()
                .iter()
                .map(|keyframe| (keyframe.at().ticks(), keyframe.value()))
                .collect()
        })
        .unwrap_or_default()
}

#[test]
fn the_first_keyframe_starts_a_lane_and_removing_it_stops_one() {
    // These two are each other's exact inverse, and that is the whole reason
    // adding to an empty lane starts it: an editor who adds a keyframe to a
    // parameter nobody has animated expects the parameter to become animated,
    // and undoing that expects it to stop — not to leave a flat curve nobody
    // asked for sitting on the track.
    let (mut project, sequence) = project();
    assert!(shape(&project, sequence).is_empty(), "it began animated");

    opacity(&mut project, sequence, KeyframeEdit::Add(key(10, 1, 4)));
    assert_eq!(shape(&project, sequence), std::vec![(10, value(1, 4))]);

    project.undo(sequence).expect("an undo");
    assert!(
        shape(&project, sequence).is_empty(),
        "undoing the first keyframe left the lane switched on"
    );

    project.redo(sequence).expect("a redo");
    assert_eq!(shape(&project, sequence), std::vec![(10, value(1, 4))]);
}

#[test]
fn every_keyframe_operation_undoes_and_redoes() {
    // The journal checks that applying an inverse reproduces the edit that was
    // applied, so a wrong inverse is `HistoryInconsistent` rather than a quiet
    // difference. This walks all four operations and then undoes the lot.
    let (mut project, sequence) = project();
    let operations = [
        KeyframeEdit::Add(key(0, 0, 1)),
        KeyframeEdit::Add(key(24, 1, 1)),
        KeyframeEdit::Add(key(12, 1, 2)),
        KeyframeEdit::Move {
            from: at(12),
            to: at(18),
        },
        KeyframeEdit::Set {
            at: at(18),
            value: value(3, 4),
            interpolation: Interpolation::Hold,
        },
        KeyframeEdit::Remove(at(0)),
    ];
    for operation in operations {
        opacity(&mut project, sequence, operation);
    }
    let after = shape(&project, sequence);
    assert_eq!(
        after,
        std::vec![(18, value(3, 4)), (24, value(1, 1))],
        "the six operations did not land where they were told to"
    );

    for step in 0..operations.len() {
        project
            .undo(sequence)
            .unwrap_or_else(|status| panic!("undo {step} refused: {status}"));
    }
    assert!(
        shape(&project, sequence).is_empty(),
        "undoing everything left something behind"
    );

    for step in 0..operations.len() {
        project
            .redo(sequence)
            .unwrap_or_else(|status| panic!("redo {step} refused: {status}"));
    }
    assert_eq!(
        shape(&project, sequence),
        after,
        "redo did not reproduce it"
    );
}

#[test]
fn a_keyframe_may_move_past_its_neighbour() {
    // Reordering is a thing an editor does on purpose, and it inverts cleanly:
    // moving back puts the curve exactly where it was, because a keyframe is
    // identified by where it *is* rather than by an index that would shift.
    let (mut project, sequence) = project();
    for frame in [0, 10, 20] {
        opacity(
            &mut project,
            sequence,
            KeyframeEdit::Add(key(frame, frame, 100)),
        );
    }
    opacity(
        &mut project,
        sequence,
        KeyframeEdit::Move {
            from: at(0),
            to: at(15),
        },
    );
    assert_eq!(
        shape(&project, sequence),
        std::vec![
            (10, value(10, 100)),
            (15, value(0, 100)),
            (20, value(20, 100))
        ],
        "the keyframe did not carry its value past its neighbour"
    );

    project.undo(sequence).expect("an undo");
    assert_eq!(
        shape(&project, sequence),
        std::vec![
            (0, value(0, 100)),
            (10, value(10, 100)),
            (20, value(20, 100))
        ]
    );
}

#[test]
fn a_move_that_changes_nothing_is_allowed_and_undoes() {
    // Dragging a keyframe and putting it back is a real gesture, and it must
    // not be a refusal — a keyframe already at its destination is not a
    // collision with itself.
    let (mut project, sequence) = project();
    opacity(&mut project, sequence, KeyframeEdit::Add(key(10, 1, 2)));
    opacity(
        &mut project,
        sequence,
        KeyframeEdit::Move {
            from: at(10),
            to: at(10),
        },
    );
    assert_eq!(shape(&project, sequence), std::vec![(10, value(1, 2))]);
    project.undo(sequence).expect("an undo");
    assert_eq!(shape(&project, sequence), std::vec![(10, value(1, 2))]);
}

#[test]
fn a_keyframe_cannot_land_on_another() {
    // Two at one instant is the same nothing as none: a parameter with two
    // values at one moment has no value.
    let (mut project, sequence) = project();
    opacity(&mut project, sequence, KeyframeEdit::Add(key(0, 0, 1)));
    opacity(&mut project, sequence, KeyframeEdit::Add(key(10, 1, 1)));

    assert_eq!(
        project.apply(
            sequence,
            Edit::Keyframe {
                track: 0,
                lane: Automation::Opacity,
                operation: KeyframeEdit::Add(key(10, 1, 2)),
            },
        ),
        Err(ModelStatus::KeyframeExists)
    );
    assert_eq!(
        project.apply(
            sequence,
            Edit::Keyframe {
                track: 0,
                lane: Automation::Opacity,
                operation: KeyframeEdit::Move {
                    from: at(0),
                    to: at(10),
                },
            },
        ),
        Err(ModelStatus::KeyframeExists)
    );
    // And a refusal changes nothing.
    assert_eq!(
        shape(&project, sequence),
        std::vec![(0, value(0, 1)), (10, value(1, 1))]
    );
}

#[test]
fn an_operation_on_a_keyframe_that_is_not_there_is_refused() {
    let (mut project, sequence) = project();
    opacity(&mut project, sequence, KeyframeEdit::Add(key(0, 0, 1)));
    opacity(&mut project, sequence, KeyframeEdit::Add(key(10, 1, 1)));

    for operation in [
        KeyframeEdit::Remove(at(5)),
        KeyframeEdit::Move {
            from: at(5),
            to: at(7),
        },
        KeyframeEdit::Set {
            at: at(5),
            value: value(1, 2),
            interpolation: Interpolation::Linear,
        },
    ] {
        assert_eq!(
            project.apply(
                sequence,
                Edit::Keyframe {
                    track: 0,
                    lane: Automation::Opacity,
                    operation,
                },
            ),
            Err(ModelStatus::NoSuchKeyframe),
            "{operation:?} was accepted against a keyframe that is not there"
        );
    }
}

#[test]
fn an_operation_on_a_lane_with_no_automation_is_refused() {
    // Everything but adding, which is what turns a lane on. Removing, moving
    // or changing a keyframe on a lane that has none is a caller confused
    // about which lane it is looking at, and saying so is more use than
    // silently starting one.
    let (mut project, sequence) = project();
    for operation in [
        KeyframeEdit::Remove(at(0)),
        KeyframeEdit::Move {
            from: at(0),
            to: at(1),
        },
        KeyframeEdit::Set {
            at: at(0),
            value: value(1, 2),
            interpolation: Interpolation::Linear,
        },
    ] {
        assert_eq!(
            project.apply(
                sequence,
                Edit::Keyframe {
                    track: 0,
                    lane: Automation::Opacity,
                    operation,
                },
            ),
            Err(ModelStatus::NoAutomation),
            "{operation:?} was accepted on a lane with nothing on it"
        );
    }
}

#[test]
fn a_lane_a_track_does_not_have_is_refused() {
    // A picture track has an opacity and a sound track has a level, and the
    // lane is named rather than inferred from the kind — so an edit written
    // for the wrong lane is refused rather than quietly applied to the only
    // one available.
    let (mut project, sequence) = project();
    assert_eq!(
        project.apply(
            sequence,
            Edit::Keyframe {
                track: 0,
                lane: Automation::Level,
                operation: KeyframeEdit::Add(key(0, 0, 1)),
            },
        ),
        Err(ModelStatus::LevelOnPicture)
    );
    assert_eq!(
        project.apply(
            sequence,
            Edit::Keyframe {
                track: 1,
                lane: Automation::Opacity,
                operation: KeyframeEdit::Add(key(0, 0, 1)),
            },
        ),
        Err(ModelStatus::OpacityOnSound)
    );
}

#[test]
fn a_keyframe_counted_another_way_is_refused() {
    let (mut project, sequence) = project();
    let other = Keyframe::new(
        Instant::new(0, Timebase::PAL_25),
        value(0, 1),
        Interpolation::Linear,
    )
    .expect("a keyframe");
    assert_eq!(
        project.apply(
            sequence,
            Edit::Keyframe {
                track: 0,
                lane: Automation::Opacity,
                operation: KeyframeEdit::Add(other),
            },
        ),
        Err(ModelStatus::WrongTimebase),
        "a keyframe counted at 25 was accepted onto a track counted at 24"
    );

    // And onto a lane that already exists, which is a different code path.
    opacity(&mut project, sequence, KeyframeEdit::Add(key(0, 0, 1)));
    assert_eq!(
        project.apply(
            sequence,
            Edit::Keyframe {
                track: 0,
                lane: Automation::Opacity,
                operation: KeyframeEdit::Add(other),
            },
        ),
        Err(ModelStatus::WrongTimebase)
    );
}

#[test]
fn setting_a_keyframe_changes_what_it_holds_and_not_when_it_is() {
    // The division of labour: `Set` cannot reorder anything and `Move` cannot
    // change a value. Two gestures, two edits, and neither can do the other's
    // damage by accident.
    let (mut project, sequence) = project();
    opacity(&mut project, sequence, KeyframeEdit::Add(key(0, 0, 1)));
    opacity(&mut project, sequence, KeyframeEdit::Add(key(10, 1, 1)));
    opacity(
        &mut project,
        sequence,
        KeyframeEdit::Set {
            at: at(0),
            value: value(1, 3),
            interpolation: Interpolation::Hold,
        },
    );
    assert_eq!(
        shape(&project, sequence),
        std::vec![(0, value(1, 3)), (10, value(1, 1))]
    );
    let curve = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .curve(Automation::Opacity)
        .expect("a curve");
    assert_eq!(curve.keyframes()[0].interpolation(), Interpolation::Hold);
    // A hold means the value does not move until the next keyframe.
    assert_eq!(curve.value_at(at(9)).expect("a value"), value(1, 3));
}

#[test]
fn an_ease_that_folds_is_refused_by_every_way_in() {
    // A horizontal handle outside its span makes a curve with more than one
    // value at an instant. Both the way in that creates a lane and the way in
    // that adds to one have to refuse it.
    let folded = Interpolation::Ease {
        out_x: value(-1, 2),
        out_y: value(0, 1),
        in_x: value(1, 2),
        in_y: value(1, 1),
    };
    assert_eq!(
        Keyframe::new(at(0), value(0, 1), folded).map(|_| ()),
        Err(ModelStatus::HandleOutOfSpan),
        "a folded ease got as far as a keyframe"
    );
}
