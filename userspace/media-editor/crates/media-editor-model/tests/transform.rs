// SPDX-License-Identifier: GPL-3.0-only
//! Transforms: what the model will hold, and what it will not.

use media_editor_core::{Digest, Duration, Rational, Timebase};
use media_editor_model::{
    Clip, Edit, Item, MediaAsset, ModelStatus, Project, Resampling, TrackKind, Transform,
};

const RATE: Timebase = Timebase::FILM_24;

fn r(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a rational")
}

fn half() -> Transform {
    Transform::scaled(
        r(1, 2),
        r(1, 2),
        (Rational::ZERO, Rational::ZERO),
        Resampling::Area,
    )
    .expect("a transform")
}

#[test]
fn a_scale_keeps_what_it_was_given() {
    let held = half();
    assert_eq!(
        held.linear(),
        [r(1, 2), Rational::ZERO, Rational::ZERO, r(1, 2)]
    );
    assert_eq!(held.offset(), (Rational::ZERO, Rational::ZERO));
    assert_eq!(held.resampling(), Resampling::Area);
}

#[test]
fn a_transform_that_flattens_the_picture_is_refused() {
    // A scale of nought on one axis, and a linear part whose rows are
    // parallel: both send every pixel onto a line, and neither has a way back.
    assert_eq!(
        Transform::scaled(
            Rational::ZERO,
            Rational::ONE,
            (Rational::ZERO, Rational::ZERO),
            Resampling::Area
        ),
        Err(ModelStatus::TransformNotInvertible)
    );
    assert_eq!(
        Transform::new(
            [Rational::ONE, r(2, 1), r(2, 1), r(4, 1)],
            (Rational::ZERO, Rational::ZERO),
            Resampling::Bilinear
        ),
        Err(ModelStatus::TransformNotInvertible)
    );
}

#[test]
fn a_mirror_is_a_transform_and_not_a_refusal() {
    // A negative determinant is a mirror, which is a thing people ask for. It
    // is the *zero* determinant that has no inverse, and confusing the two
    // would refuse flipping a shot.
    assert!(
        Transform::scaled(
            r(-1, 1),
            Rational::ONE,
            (Rational::ZERO, Rational::ZERO),
            Resampling::Area
        )
        .is_ok()
    );
}

#[test]
fn the_identity_is_still_and_a_move_is_not() {
    // A clip nobody has transformed has to be recognisable as such, so the
    // renderer can skip the resampler entirely rather than running one that
    // happens to be the identity.
    let still = Transform::new(
        [Rational::ONE, Rational::ZERO, Rational::ZERO, Rational::ONE],
        (Rational::ZERO, Rational::ZERO),
        Resampling::Area,
    )
    .expect("a transform");
    assert!(still.is_still());
    assert!(!half().is_still());

    let nudged = Transform::new(
        [Rational::ONE, Rational::ZERO, Rational::ZERO, Rational::ONE],
        (r(1, 100), Rational::ZERO),
        Resampling::Area,
    )
    .expect("a transform");
    assert!(!nudged.is_still(), "a move of a hundredth is still a move");
}

/// A project with one clip on one picture track.
fn project() -> (Project, media_editor_model::SequenceId) {
    let mut project = Project::new();
    let media = project
        .add_media(
            MediaAsset::new(
                Digest::of(b"moved"),
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

fn transform_of(project: &Project, sequence: media_editor_model::SequenceId) -> Option<Transform> {
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
    clip.transform()
}

#[test]
fn a_transform_is_set_by_an_edit_and_undone_by_its_inverse() {
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: Some(half()),
            },
        )
        .expect("a transform");
    assert_eq!(transform_of(&project, sequence), Some(half()));
    project.undo(sequence).expect("an undo");
    assert_eq!(transform_of(&project, sequence), None);
    project.redo(sequence).expect("a redo");
    assert_eq!(transform_of(&project, sequence), Some(half()));
}

#[test]
fn a_transform_survives_a_slip_and_a_trim() {
    // The rebuild trap, met by the third field added to a clip. `with_source`
    // and `with_duration` keep everything else, and this is what says so.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: Some(half()),
            },
        )
        .expect("a transform");
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
    assert_eq!(
        transform_of(&project, sequence),
        Some(half()),
        "after a slip"
    );
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
    assert_eq!(
        transform_of(&project, sequence),
        Some(half()),
        "after a trim"
    );
}

#[test]
fn a_gap_cannot_be_moved() {
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
            Edit::SetClipTransform {
                track: 0,
                index: 1,
                transform: Some(half()),
            },
        ),
        Err(ModelStatus::NotAClip)
    );
}

#[test]
fn the_layer_stack_carries_the_transform_up() {
    use media_editor_core::Instant;
    use media_editor_model::Lane;

    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: Some(half()),
            },
        )
        .expect("a transform");
    let stack = project
        .sequence(sequence)
        .expect("a sequence")
        .stack_at(Lane::Picture, Instant::new(10, RATE))
        .expect("a stack");
    assert_eq!(stack[0].transform(), Some(half()));
}
