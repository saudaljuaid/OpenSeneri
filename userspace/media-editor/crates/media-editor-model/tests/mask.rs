// SPDX-License-Identifier: GPL-3.0-only
//! Masks: the shapes the model will hold, and the ones it will not.

use media_editor_core::{Digest, Duration, Rational, Timebase};
use media_editor_model::{Clip, Edit, Item, Mask, MediaAsset, ModelStatus, Project, TrackKind};

const RATE: Timebase = Timebase::FILM_24;

fn r(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a rational")
}

fn square() -> Mask {
    Mask::rectangle(r(1, 4), r(1, 4), r(3, 4), r(3, 4)).expect("a rectangle")
}

#[test]
fn a_rectangle_is_four_corners_in_order() {
    let held = square();
    assert_eq!(held.corners().len(), 4);
    assert_eq!(held.corners()[0], (r(1, 4), r(1, 4)));
    assert_eq!(held.corners()[2], (r(3, 4), r(3, 4)));
    assert!(!held.is_inverted());
}

#[test]
fn either_winding_is_accepted() {
    // An editor drags points in whatever order the shape came out. Insisting
    // on one direction round would refuse half the shapes anybody draws, and
    // the renderer can measure which way they run.
    let clockwise = Mask::new(vec![
        (r(0, 1), r(0, 1)),
        (r(1, 1), r(0, 1)),
        (r(1, 1), r(1, 1)),
    ])
    .expect("a triangle");
    let widdershins = Mask::new(vec![
        (r(1, 1), r(1, 1)),
        (r(1, 1), r(0, 1)),
        (r(0, 1), r(0, 1)),
    ])
    .expect("the same triangle the other way");
    assert_ne!(clockwise, widdershins, "they are different values");
}

#[test]
fn a_concave_outline_is_refused_rather_than_repaired() {
    // The arrowhead: four corners, one of which turns the other way. Quietly
    // taking its convex hull would be a different shape, drawn by nobody, and
    // impossible to notice until something went to air.
    let arrow = Mask::new(vec![
        (r(0, 1), r(0, 1)),
        (r(1, 2), r(1, 4)),
        (r(1, 1), r(0, 1)),
        (r(1, 2), r(1, 1)),
    ]);
    assert_eq!(arrow, Err(ModelStatus::MaskNotConvex));
}

#[test]
fn corners_in_a_line_enclose_nothing() {
    let flat = Mask::new(vec![
        (r(0, 1), r(0, 1)),
        (r(1, 2), r(1, 2)),
        (r(1, 1), r(1, 1)),
    ]);
    assert_eq!(flat, Err(ModelStatus::MaskTooSimple));
}

#[test]
fn a_corner_that_does_not_turn_is_allowed() {
    // Five points where the middle one sits on an edge: the same region as the
    // four-cornered version. Refusing it would refuse a rectangle somebody
    // built by dragging a fifth point onto a side.
    let held = Mask::new(vec![
        (r(0, 1), r(0, 1)),
        (r(1, 2), r(0, 1)),
        (r(1, 1), r(0, 1)),
        (r(1, 1), r(1, 1)),
        (r(0, 1), r(1, 1)),
    ]);
    assert!(held.is_ok());
}

#[test]
fn two_corners_do_not_make_a_shape() {
    assert_eq!(
        Mask::new(vec![(r(0, 1), r(0, 1)), (r(1, 1), r(1, 1))]),
        Err(ModelStatus::MaskTooSimple)
    );
}

#[test]
fn a_rectangle_enclosing_nothing_is_refused() {
    assert_eq!(
        Mask::rectangle(r(1, 2), r(0, 1), r(1, 2), r(1, 1)),
        Err(ModelStatus::MaskTooSimple)
    );
}

#[test]
fn inverting_twice_is_the_shape_again() {
    let held = square();
    assert_eq!(held.inverted().inverted(), held);
    assert!(held.inverted().is_inverted());
    assert_eq!(held.inverted().corners(), held.corners());
}

/// A project with one clip on one picture track.
fn project() -> (Project, media_editor_model::SequenceId) {
    let mut project = Project::new();
    let media = project
        .add_media(
            MediaAsset::new(
                Digest::of(b"masked"),
                RATE,
                Duration::new(9_000, RATE).expect("a length"),
            )
            .expect("an asset"),
        )
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
                item: Item::Clip(
                    Clip::new(media, 0, Duration::new(48, RATE).expect("a length"))
                        .expect("a clip"),
                ),
            },
        )
        .expect("a clip");
    project.forget_history();
    (project, sequence)
}

fn mask_of(project: &Project, sequence: media_editor_model::SequenceId) -> Option<Mask> {
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
    clip.mask().cloned()
}

#[test]
fn a_mask_is_set_by_an_edit_and_undone_by_its_inverse() {
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipMask {
                track: 0,
                index: 0,
                mask: Some(square()),
            },
        )
        .expect("a mask");
    assert_eq!(mask_of(&project, sequence), Some(square()));
    project.undo(sequence).expect("an undo");
    assert_eq!(mask_of(&project, sequence), None);
    project.redo(sequence).expect("a redo");
    assert_eq!(mask_of(&project, sequence), Some(square()));
}

#[test]
fn a_mask_survives_a_slip_and_a_trim() {
    // The rebuild trap the grade already found once, met again by the field
    // added after it. `with_source` and `with_duration` keep everything else,
    // and this is what says so rather than a comment.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipMask {
                track: 0,
                index: 0,
                mask: Some(square()),
            },
        )
        .expect("a mask");
    project
        .apply(
            sequence,
            Edit::SetClipSource {
                track: 0,
                index: 0,
                source_start: 12,
            },
        )
        .expect("a slip");
    assert_eq!(mask_of(&project, sequence), Some(square()), "after a slip");
    project
        .apply(
            sequence,
            Edit::SetItemDuration {
                track: 0,
                index: 0,
                duration: Duration::new(24, RATE).expect("a length"),
            },
        )
        .expect("a trim");
    assert_eq!(mask_of(&project, sequence), Some(square()), "after a trim");
}

#[test]
fn a_gap_cannot_be_masked() {
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 1,
                item: Item::gap(Duration::new(24, RATE).expect("a length")).expect("a gap"),
            },
        )
        .expect("a gap");
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipMask {
                track: 0,
                index: 1,
                mask: Some(square()),
            },
        ),
        Err(ModelStatus::NotAClip),
        "accepting it and doing nothing would make the inverse a lie"
    );
}

#[test]
fn the_layer_stack_carries_the_mask_up() {
    use media_editor_core::Instant;
    use media_editor_model::Lane;

    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipMask {
                track: 0,
                index: 0,
                mask: Some(square().inverted()),
            },
        )
        .expect("a mask");
    let stack = project
        .sequence(sequence)
        .expect("a sequence")
        .stack_at(Lane::Picture, Instant::new(10, RATE))
        .expect("a stack");
    assert_eq!(stack.len(), 1);
    let carried = stack[0].mask().expect("a mask");
    assert_eq!(carried.corners(), square().corners());
    assert!(carried.is_inverted());
}
