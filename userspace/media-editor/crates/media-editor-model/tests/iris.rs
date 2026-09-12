// SPDX-License-Identifier: GPL-3.0-only
//! Mask animation for irises, vignettes, and reveals.
//!
//! Animation uses positive scale and translation around the mask centroid, so
//! convex masks remain convex throughout the move.

use media_editor_core::{Digest, Duration, Instant, Rational, Timebase};
use media_editor_model::{
    Clip, Curve, Edit, Interpolation, Item, Keyframe, Lane, Mask, MediaAsset, ModelStatus, Motion,
    Project, SequenceId, TrackKind, Turn,
};

const RATE: Timebase = Timebase::FILM_24;
const LENGTH: i64 = 24;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a length")
}

fn r(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a rational")
}

fn at(tick: i64) -> Instant {
    Instant::new(tick, RATE)
}

/// A straight run from `from` to `to` over `over` ticks.
fn ramp(from: Rational, to: Rational, over: i64) -> Curve {
    Curve::new(std::vec![
        Keyframe::new(at(0), from, Interpolation::Linear).expect("a keyframe"),
        Keyframe::new(at(over), to, Interpolation::Linear).expect("a keyframe"),
    ])
    .expect("a curve")
}

/// A trapezoid whose corners average to one point and whose *area* balances
/// on another: the shape that tells the two apart.
fn trapezoid() -> Mask {
    Mask::new(std::vec![
        (Rational::ZERO, Rational::ZERO),
        (Rational::ONE, Rational::ZERO),
        (r(3, 4), Rational::ONE),
        (r(1, 4), Rational::ONE),
    ])
    .expect("a trapezoid")
}

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
                item: Item::Clip(Clip::new(media, 100, frames(LENGTH)).expect("a clip")),
            },
        )
        .expect("a clip");
    project.forget_history();
    (project, sequence)
}

fn clip_at(project: &Project, sequence: SequenceId, index: usize) -> Clip {
    let Item::Clip(held) = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(index)
        .expect("an item")
    else {
        panic!("a clip");
    };
    held.clone()
}

#[test]
fn a_shape_balances_on_its_area_and_not_on_its_corners() {
    // The trapezoid's four corners average to (1/2, 1/2). Its area balances at
    // (1/2, 4/9), derived by hand from the definition: for parallel sides `a`
    // at the bottom and `b` at the top, the centroid sits `(a + 2b)/(3(a + b))`
    // of the way up, which for a = 1 and b = 1/2 is 2/(3 x 3/2) = 4/9.
    //
    // A shape whose corners are evenly spread gives the same answer either
    // way, which is why the fixture is a trapezoid and not a rectangle.
    assert_eq!(
        trapezoid().centroid().expect("a centroid"),
        (r(1, 2), r(4, 9))
    );
    assert_ne!(
        trapezoid().centroid().expect("a centroid"),
        (r(1, 2), r(1, 2)),
        "which is where the corners average to"
    );
}

#[test]
fn a_shape_wound_either_way_balances_on_the_same_point() {
    // The cross products change sign together with the area they weigh, so
    // the sign cancels. Worth a test because a winding-dependent centroid
    // would move a mask the moment somebody dragged its points the other way
    // round -- and either winding is accepted, deliberately.
    let mut backwards = trapezoid().corners().to_vec();
    backwards.reverse();
    assert_eq!(
        Mask::new(backwards)
            .expect("a mask")
            .centroid()
            .expect("a centroid"),
        trapezoid().centroid().expect("a centroid")
    );
}

#[test]
fn a_shape_scaled_by_one_and_moved_by_nothing_is_itself() {
    // The neutral values, exactly -- not nearly. A mask that drifted by a
    // rounding every frame it was "not animated" would drift a long way over
    // a programme.
    let held = trapezoid()
        .moved_by(Rational::ONE, Rational::ZERO, Rational::ZERO, Turn::NONE)
        .expect("no change");
    assert_eq!(held.corners(), trapezoid().corners());
}

#[test]
fn a_shape_grows_in_place_rather_than_sliding_to_the_middle() {
    // A small square in the top left, doubled. About its own centroid it stays
    // in the top left and gets bigger; about the frame's centre it would slide
    // down and right while growing, which is the fault this test exists for.
    let corner = Mask::rectangle(r(1, 10), r(1, 10), r(3, 10), r(3, 10)).expect("a square");
    assert_eq!(corner.centroid().expect("a centroid"), (r(2, 10), r(2, 10)));
    let bigger = corner
        .moved_by(r(2, 1), Rational::ZERO, Rational::ZERO, Turn::NONE)
        .expect("doubled");
    assert_eq!(
        bigger.centroid().expect("a centroid"),
        (r(2, 10), r(2, 10)),
        "the middle has not moved"
    );
    assert_eq!(
        bigger.corners(),
        [
            (Rational::ZERO, Rational::ZERO),
            (r(4, 10), Rational::ZERO),
            (r(4, 10), r(4, 10)),
            (Rational::ZERO, r(4, 10)),
        ],
        "and it is twice the size about that middle"
    );
}

#[test]
fn a_scale_of_nothing_or_less_is_refused() {
    // At nought the shape collapses to a point with no area, which `Mask::new`
    // would refuse if it were built from those corners; below nought it turns
    // inside out through its own middle, which is a mirror and belongs in the
    // corners somebody drew.
    for scale in [Rational::ZERO, r(-1, 1)] {
        assert_eq!(
            trapezoid().moved_by(scale, Rational::ZERO, Rational::ZERO, Turn::NONE),
            Err(ModelStatus::ScaleNotPositive)
        );
    }
}

#[test]
fn a_clip_with_no_animation_hands_out_the_shape_it_was_given() {
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipMask {
                track: 0,
                index: 0,
                mask: Some(trapezoid()),
            },
        )
        .expect("a mask");
    let held = clip_at(&project, sequence, 0);
    assert_eq!(held.mask_at(0).expect("a shape"), Some(trapezoid()));
    assert_eq!(
        held.mask_at(LENGTH - 1).expect("a shape"),
        Some(trapezoid())
    );
}

#[test]
fn the_layer_stack_hands_out_the_resolved_shape() {
    // Resolved rather than a shape and a curve beside it, exactly as the
    // framing is: by the time a layer describes a frame the animation has
    // already become the shape it reads at that moment, and nothing below has
    // to be told there was a curve. A renderer that had to be would need a
    // clock, and a node that depends on a clock has a cache key that lies.
    let (mut project, sequence) = project();
    for edit in [
        Edit::SetClipMask {
            track: 0,
            index: 0,
            mask: Some(Mask::rectangle(r(1, 4), r(1, 4), r(3, 4), r(3, 4)).expect("a square")),
        },
        Edit::SetClipMaskMotion {
            track: 0,
            index: 0,
            motion: Some(
                Motion::new(Some(ramp(Rational::ONE, r(2, 1), 12)), None, None, None)
                    .expect("an iris"),
            ),
        },
    ] {
        project.apply(sequence, edit).expect("an edit");
    }
    let shape = |tick| {
        project
            .sequence(sequence)
            .expect("a sequence")
            .stack_at(Lane::Picture, at(tick))
            .expect("a stack")[0]
            .mask()
            .cloned()
            .expect("a shape")
    };
    // A half-frame square about (1/2, 1/2), scaled 1 -> 2 over twelve frames.
    assert_eq!(shape(0).corners()[0], (r(1, 4), r(1, 4)));
    assert_eq!(
        shape(6).corners()[0],
        (r(1, 8), r(1, 8)),
        "half way, the square is three quarters of the frame"
    );
    assert_eq!(
        shape(12).corners()[0],
        (Rational::ZERO, Rational::ZERO),
        "and at the end it covers all of it"
    );
}

#[test]
fn a_shape_can_sweep_a_card_on_from_its_left_edge() {
    // A left-to-right reveal combines mask scale and translation because
    // scaling about the centroid alone grows in both directions.
    //
    // A strip from nought to a quarter, scaled by `s` about its middle at 1/8,
    // has its left edge at `1/8 - s/8`. Moving it right by `(s - 1)/8` puts
    // that edge back at nought for every `s`, so the strip grows rightwards
    // only. At s = 4 it is the whole frame.
    let (mut project, sequence) = project();
    for edit in [
        Edit::SetClipMask {
            track: 0,
            index: 0,
            mask: Some(
                Mask::rectangle(Rational::ZERO, Rational::ZERO, r(1, 4), Rational::ONE)
                    .expect("a strip"),
            ),
        },
        Edit::SetClipMaskMotion {
            track: 0,
            index: 0,
            motion: Some(
                Motion::new(
                    Some(ramp(Rational::ONE, r(4, 1), 12)),
                    Some(ramp(Rational::ZERO, r(3, 8), 12)),
                    None,
                    None,
                )
                .expect("a sweep"),
            ),
        },
    ] {
        project.apply(sequence, edit).expect("an edit");
    }
    let held = clip_at(&project, sequence, 0);
    for (tick, right) in [
        (0, r(1, 4)),
        (4, r(1, 2)),
        (8, r(3, 4)),
        (12, Rational::ONE),
    ] {
        let shape = held.mask_at(tick).expect("a shape").expect("a mask");
        assert_eq!(
            shape.corners()[0].0,
            Rational::ZERO,
            "the left edge has not moved, at {tick}"
        );
        assert_eq!(
            shape.corners()[1].0,
            right,
            "and the right edge is at {tick}"
        );
    }
}

#[test]
fn an_animation_of_a_mask_that_is_not_there_is_refused() {
    let (mut project, sequence) = project();
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipMaskMotion {
                track: 0,
                index: 0,
                motion: Some(
                    Motion::new(Some(ramp(Rational::ONE, r(2, 1), 12)), None, None, None)
                        .expect("an iris")
                ),
            },
        ),
        Err(ModelStatus::NoMaskToAnimate)
    );
}

#[test]
fn taking_the_shape_off_an_animated_clip_is_refused() {
    // Dropping the animation here instead would discard work the caller did
    // not name -- and make the inverse a lie, because undoing would put the
    // shape back with the animation already gone.
    let (mut project, sequence) = project();
    for edit in [
        Edit::SetClipMask {
            track: 0,
            index: 0,
            mask: Some(trapezoid()),
        },
        Edit::SetClipMaskMotion {
            track: 0,
            index: 0,
            motion: Some(
                Motion::new(Some(ramp(Rational::ONE, r(2, 1), 12)), None, None, None)
                    .expect("an iris"),
            ),
        },
    ] {
        project.apply(sequence, edit).expect("an edit");
    }
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipMask {
                track: 0,
                index: 0,
                mask: None,
            },
        ),
        Err(ModelStatus::NoMaskToAnimate)
    );
}

#[test]
fn a_cut_re_bases_the_shape_animation_onto_the_tail() {
    // The same fault the framing had, in a third place. A curve measured from
    // its clip's start, carried unchanged onto a tail whose start is later,
    // restarts the animation at the cut.
    let (mut project, sequence) = project();
    for edit in [
        Edit::SetClipMask {
            track: 0,
            index: 0,
            mask: Some(Mask::rectangle(r(1, 4), r(1, 4), r(3, 4), r(3, 4)).expect("a square")),
        },
        Edit::SetClipMaskMotion {
            track: 0,
            index: 0,
            motion: Some(
                Motion::new(Some(ramp(Rational::ONE, r(2, 1), 20)), None, None, None)
                    .expect("an iris"),
            ),
        },
    ] {
        project.apply(sequence, edit).expect("an edit");
    }
    let whole: std::vec::Vec<_> = (0..LENGTH)
        .map(|offset| {
            clip_at(&project, sequence, 0)
                .mask_at(offset)
                .expect("a shape")
        })
        .collect();
    project
        .apply(
            sequence,
            Edit::SplitItem {
                track: 0,
                index: 0,
                offset: 8,
            },
        )
        .expect("a cut");
    let tail = clip_at(&project, sequence, 1);
    for offset in 0..(LENGTH - 8) {
        assert_eq!(
            tail.mask_at(offset).expect("a shape"),
            whole[usize::try_from(offset + 8).expect("an index")],
            "the tail at {offset} shows what the whole showed at {}",
            offset + 8
        );
    }
}

#[test]
fn two_halves_of_an_animated_shape_join_back_into_one() {
    let (mut project, sequence) = project();
    let iris =
        Motion::new(Some(ramp(Rational::ONE, r(2, 1), 20)), None, None, None).expect("an iris");
    for edit in [
        Edit::SetClipMask {
            track: 0,
            index: 0,
            mask: Some(Mask::rectangle(r(1, 4), r(1, 4), r(3, 4), r(3, 4)).expect("a square")),
        },
        Edit::SetClipMaskMotion {
            track: 0,
            index: 0,
            motion: Some(iris.clone()),
        },
        Edit::SplitItem {
            track: 0,
            index: 0,
            offset: 8,
        },
        Edit::JoinItems { track: 0, index: 0 },
    ] {
        project.apply(sequence, edit).expect("an edit");
    }
    let whole = clip_at(&project, sequence, 0);
    assert_eq!(whole.duration(), frames(LENGTH));
    assert_eq!(whole.mask_motion(), Some(&iris));
}

#[test]
fn two_clips_whose_shape_animations_do_not_line_up_do_not_join() {
    let (mut project, sequence) = project();
    for edit in [
        Edit::SetClipMask {
            track: 0,
            index: 0,
            mask: Some(trapezoid()),
        },
        Edit::SetClipMaskMotion {
            track: 0,
            index: 0,
            motion: Some(
                Motion::new(Some(ramp(Rational::ONE, r(2, 1), 12)), None, None, None)
                    .expect("an iris"),
            ),
        },
    ] {
        project.apply(sequence, edit).expect("an edit");
    }
    let first = clip_at(&project, sequence, 0);
    let second = first
        .with_source(100 + LENGTH)
        .expect("the frames after")
        .clone();
    assert!(
        !Item::Clip(first.clone()).continues_into(&Item::Clip(second)),
        "an animation that restarts at the cut is not one animation"
    );
}

#[test]
fn the_framing_and_the_shape_animate_independently() {
    // A vignette should stay where it was put while the shot pushes in, and a
    // mask glued to the picture is what *tracking* wants. Two animations,
    // because they are two questions -- and this is the test that would notice
    // if one field had been made to serve both.
    let (mut project, sequence) = project();
    for edit in [
        Edit::SetClipTransform {
            track: 0,
            index: 0,
            transform: Some(
                media_editor_model::Transform::scaled(
                    Rational::ONE,
                    Rational::ONE,
                    (Rational::ZERO, Rational::ZERO),
                    media_editor_model::Resampling::Bilinear,
                )
                .expect("a framing"),
            ),
        },
        Edit::SetClipMotion {
            track: 0,
            index: 0,
            motion: Some(
                Motion::new(Some(ramp(Rational::ONE, r(2, 1), 12)), None, None, None)
                    .expect("a push"),
            ),
        },
        Edit::SetClipMask {
            track: 0,
            index: 0,
            mask: Some(Mask::rectangle(r(1, 4), r(1, 4), r(3, 4), r(3, 4)).expect("a square")),
        },
    ] {
        project.apply(sequence, edit).expect("an edit");
    }
    let held = clip_at(&project, sequence, 0);
    // The picture pushes in over twelve frames; the shape does not move.
    assert_eq!(
        held.mask_at(0).expect("a shape"),
        held.mask_at(12).expect("a shape"),
        "the vignette stayed where it was put"
    );
    let stack = project
        .sequence(sequence)
        .expect("a sequence")
        .stack_at(Lane::Picture, at(12))
        .expect("a stack");
    assert_eq!(
        stack[0].transform().expect("a framing").linear()[0],
        r(2, 1),
        "while the picture is twice the size"
    );
}
