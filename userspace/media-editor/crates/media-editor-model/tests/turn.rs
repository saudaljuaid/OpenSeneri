// SPDX-License-Identifier: GPL-3.0-only
//! Exact rational rotations.
//!
//! A half-angle parameter maps rational values onto the unit circle with
//! `cos = (1 - t²)/(1 + t²)` and `sin = 2t/(1 + t²)`.

use media_editor_core::{Digest, Duration, Instant, Rational, Timebase};
use media_editor_model::{
    Clip, Curve, Edit, Interpolation, Item, Keyframe, Lane, Mask, MediaAsset, ModelStatus, Motion,
    Project, Resampling, SequenceId, TrackKind, Transform, Turn,
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

/// A curve running from `from` to `to` over the clip, straight.
fn ramp(from: Rational, to: Rational) -> Curve {
    Curve::new(std::vec![
        Keyframe::new(at(0), from, Interpolation::Linear).expect("a keyframe"),
        Keyframe::new(at(LENGTH), to, Interpolation::Linear).expect("a keyframe"),
    ])
    .expect("a curve")
}

#[test]
fn the_half_angle_reaches_exact_rotations_nobody_had_to_approximate() {
    // Each of these is derived from `cos = (1 - t^2)/(1 + t^2)` and
    // `sin = 2t/(1 + t^2)` by hand, not read back out of the function.
    //
    // t = 0    -> (1, 0)          no turn
    // t = 1    -> (0, 1)          a quarter turn: (1-1)/2 = 0, 2/2 = 1
    // t = 1/3  -> (4/5, 3/5)      (1-1/9)/(1+1/9) = (8/9)/(10/9) = 4/5,
    //                             (2/3)/(10/9) = 6/10 = 3/5 -- the 3-4-5
    //                             triangle, about 36.87 degrees
    // t = 1/2  -> (3/5, 4/5)      the same triangle the other way up
    // t = -1   -> (0, -1)         a quarter turn the other way
    for (parameter, cosine, sine) in [
        (Rational::ZERO, Rational::ONE, Rational::ZERO),
        (Rational::ONE, Rational::ZERO, Rational::ONE),
        (r(1, 3), r(4, 5), r(3, 5)),
        (r(1, 2), r(3, 5), r(4, 5)),
        (r(-1, 1), Rational::ZERO, r(-1, 1)),
    ] {
        let turn = Turn::from_half_angle(parameter).expect("a turn");
        assert_eq!(
            (turn.cosine(), turn.sine()),
            (cosine, sine),
            "the half-angle formula is wrong at {parameter:?}"
        );
    }
}

#[test]
fn every_turn_is_on_the_circle_and_anything_else_is_refused() {
    // The invariant that makes this a rotation rather than a matrix. A pair
    // off the circle is a scale smuggled in through a rotation's name, and a
    // caller who wanted a scale has a lane for it where it is checked.
    assert_eq!(
        Turn::new(r(1, 2), r(1, 2)).expect_err("a refusal"),
        ModelStatus::NotATurn,
        "a half and a half sum to a half, not to one"
    );
    assert_eq!(
        Turn::new(r(6, 5), r(8, 5)).expect_err("a refusal"),
        ModelStatus::NotATurn,
        "a three-four-five scaled up is still not on the circle"
    );
    // And the ones that are on it are accepted, including the half turn no
    // finite parameter reaches.
    for (cosine, sine) in [
        (Rational::ONE, Rational::ZERO),
        (r(-1, 1), Rational::ZERO),
        (r(3, 5), r(4, 5)),
        (r(-5, 13), r(12, 13)),
    ] {
        assert!(
            Turn::new(cosine, sine).is_ok(),
            "{cosine:?}, {sine:?} is on the circle"
        );
    }
    // The half turn is the point of that list. It is the one rotation the
    // parameter cannot express, which is why the type stores the point.
    let mut reached = false;
    for numerator in -400_i64..=400 {
        let parameter = r(numerator, 7);
        let turn = Turn::from_half_angle(parameter).expect("a turn");
        reached |= turn.cosine() == r(-1, 1);
    }
    assert!(
        !reached,
        "a finite parameter reached the half turn, so the argument for storing \
         the point rather than the parameter is wrong"
    );
}

#[test]
fn turns_compose_without_drifting_off_the_circle() {
    // The property a floating-point rotation does not have. Composing the
    // three-four-five turn with itself four times stays exactly on the circle
    // -- no renormalisation, because the Brahmagupta identity is exact over
    // rationals.
    let step = Turn::new(r(4, 5), r(3, 5)).expect("a turn");
    let mut turned = Turn::NONE;
    for _ in 0..4 {
        turned = turned.composed_with(step).expect("a composition");
        let (c, s) = (turned.cosine(), turned.sine());
        assert_eq!(
            c.checked_mul(c)
                .and_then(|cc| s.checked_mul(s).and_then(|ss| cc.checked_add(ss)))
                .expect("a sum"),
            Rational::ONE,
            "the composition left the circle"
        );
    }
    // Two quarter turns are a half turn, exactly -- which is also the second
    // way to reach the point no parameter does.
    let quarter = Turn::from_half_angle(Rational::ONE).expect("a turn");
    let half = quarter.composed_with(quarter).expect("a composition");
    assert_eq!((half.cosine(), half.sine()), (r(-1, 1), Rational::ZERO));
    // And four of them are back where they started.
    let whole = half.composed_with(half).expect("a composition");
    assert_eq!(whole, Turn::NONE);
    assert!(whole.is_still());
}

#[test]
fn the_denominators_are_what_bounds_a_composed_animation() {
    // The measured reason a motion animates the *parameter* rather than
    // composing a fixed small turn once per frame. That would be exact and
    // would be constant angular speed, and it is unrepresentable: the
    // denominators multiply.
    let step = Turn::new(r(4, 5), r(3, 5)).expect("a turn");
    let mut turned = Turn::NONE;
    let mut survived = 0;
    for _ in 0..64 {
        match turned.composed_with(step) {
            Ok(next) => {
                turned = next;
                survived += 1;
            }
            Err(_) => break,
        }
    }
    assert!(
        survived < 32,
        "a fifth of a right angle composed {survived} times still fits, so the \
         argument in `from_half_angle` understates what an i64 holds"
    );
    assert!(
        survived > 4,
        "and it must survive long enough that the bound is arithmetic rather \
         than an immediate refusal"
    );
}

#[test]
fn a_turn_of_nothing_leaves_a_shape_corner_for_corner() {
    // Exact rather than nearly: a shape nobody turned must come back the shape
    // it was. This is the same promise `Transform::is_still` makes about the
    // resampler.
    let shape = Mask::new(std::vec![
        (r(1, 4), r(1, 4)),
        (r(3, 4), r(1, 3)),
        (r(2, 3), r(7, 8)),
        (r(1, 5), r(3, 4)),
    ])
    .expect("a mask");
    assert_eq!(
        shape
            .moved_by(Rational::ONE, Rational::ZERO, Rational::ZERO, Turn::NONE)
            .expect("a mask"),
        shape
    );
}

#[test]
fn a_shape_turns_about_its_own_centroid() {
    // About the shape's own balance point rather than the frame's middle, for
    // the reason the scale is: a vignette that turned about the frame's centre
    // would swing across the picture rather than rotating in place.
    //
    // A quarter turn about the centroid sends the corner offset `(dx, dy)` to
    // `(-dy, dx)`, which is derived from the rotation matrix by hand.
    let shape = Mask::new(std::vec![
        (r(1, 4), r(1, 4)),
        (r(3, 4), r(1, 4)),
        (r(3, 4), r(3, 4)),
        (r(1, 4), r(3, 4)),
    ])
    .expect("a mask");
    let (cx, cy) = shape.centroid().expect("a centroid");
    assert_eq!(
        (cx, cy),
        (r(1, 2), r(1, 2)),
        "a square balances in its middle"
    );

    let quarter = Turn::from_half_angle(Rational::ONE).expect("a turn");
    let turned = shape
        .moved_by(Rational::ONE, Rational::ZERO, Rational::ZERO, quarter)
        .expect("a mask");
    for (before, after) in shape.corners().iter().zip(turned.corners()) {
        let (dx, dy) = (
            before.0.checked_sub(cx).expect("a difference"),
            before.1.checked_sub(cy).expect("a difference"),
        );
        assert_eq!(
            *after,
            (
                cx.checked_sub(dy).expect("a sum"),
                cy.checked_add(dx).expect("a sum")
            ),
            "a corner did not land where a quarter turn puts it"
        );
    }
    // And the centroid is where it was, which is what "about its own centroid"
    // means and is not implied by the corner arithmetic above.
    assert_eq!(turned.centroid().expect("a centroid"), (cx, cy));
}

#[test]
fn a_turn_preserves_area_and_a_scale_commutes_with_it() {
    // Two properties that together say why the turn needed no refusal of its
    // own: the determinant is one, so it cannot collapse a shape the scale did
    // not already collapse; and a scalar commutes with a rotation, so the
    // order the two are applied in about the same point does not matter.
    let shape = Mask::new(std::vec![
        (r(1, 5), r(1, 4)),
        (r(4, 5), r(1, 3)),
        (r(3, 5), r(7, 8)),
    ])
    .expect("a mask");
    let turn = Turn::new(r(3, 5), r(4, 5)).expect("a turn");

    let scaled_then_turned = shape
        .moved_by(Rational::ONE, Rational::ZERO, Rational::ZERO, turn)
        .expect("a mask")
        .moved_by(r(3, 2), Rational::ZERO, Rational::ZERO, Turn::NONE)
        .expect("a mask");
    let together = shape
        .moved_by(r(3, 2), Rational::ZERO, Rational::ZERO, turn)
        .expect("a mask");
    assert_eq!(
        scaled_then_turned, together,
        "a scalar and a rotation about one point stopped commuting"
    );

    // Area, exactly. The shoelace sum is what `centroid` divides by, so this
    // computes it the same way and compares the two shapes' magnitudes.
    let twice_area = |mask: &Mask| -> Rational {
        let corners = mask.corners();
        let mut sum = Rational::ZERO;
        for index in 0..corners.len() {
            let (x0, y0) = corners[index];
            let (x1, y1) = corners[(index + 1) % corners.len()];
            sum = sum
                .checked_add(
                    x0.checked_mul(y1)
                        .and_then(|left| {
                            y0.checked_mul(x1).and_then(|right| left.checked_sub(right))
                        })
                        .expect("a cross product"),
                )
                .expect("a sum");
        }
        sum
    };
    let turned_only = shape
        .moved_by(Rational::ONE, Rational::ZERO, Rational::ZERO, turn)
        .expect("a mask");
    assert_eq!(
        twice_area(&turned_only),
        twice_area(&shape),
        "a turn changed the area, so its determinant is not one"
    );
}

#[test]
fn a_shape_that_turns_stays_convex_and_the_move_comes_last() {
    // Convexity is what lets `moved_by` hand back a mask rather than a result
    // that could refuse mid-render, and `Mask::new` is what enforces it -- so
    // rebuilding the turned corners through it is the check.
    let shape = Mask::new(std::vec![
        (r(1, 10), r(1, 10)),
        (r(9, 10), r(1, 5)),
        (r(4, 5), r(9, 10)),
        (r(1, 5), r(4, 5)),
    ])
    .expect("a mask");
    for numerator in -8_i64..=8 {
        let turn = Turn::from_half_angle(r(numerator, 4)).expect("a turn");
        let turned = shape
            .moved_by(r(1, 2), r(1, 8), r(-1, 8), turn)
            .expect("a mask");
        assert!(
            Mask::new(turned.corners().to_vec()).is_ok(),
            "a turn of {numerator}/4 made the outline something Mask::new refuses"
        );
        // The move is added after the turn, so the centroid lands exactly the
        // move away from where it was. Turning the moved shape instead would
        // swing the whole thing around the origin.
        let (cx, cy) = shape.centroid().expect("a centroid");
        assert_eq!(
            turned.centroid().expect("a centroid"),
            (
                cx.checked_add(r(1, 8)).expect("a sum"),
                cy.checked_add(r(-1, 8)).expect("a sum")
            ),
            "the move was applied before the turn"
        );
    }
}

#[test]
fn a_turn_acts_on_the_left_of_the_framing() {
    // `R·M` turns the picture as the viewer sees it; `M·R` turns the source
    // before the framing is applied. They differ exactly when the framing is
    // not itself a rotation -- a mirror is the case that separates them, and
    // it is the case an editor actually has.
    let mirrored = Transform::scaled(
        r(-1, 1),
        Rational::ONE,
        (Rational::ZERO, Rational::ZERO),
        Resampling::Bilinear,
    )
    .expect("a transform");
    let quarter = Turn::from_half_angle(Rational::ONE).expect("a turn");
    let turned = mirrored
        .moved_by(Rational::ONE, Rational::ZERO, Rational::ZERO, quarter)
        .expect("a framing");
    // R = [0 -1; 1 0], M = [-1 0; 0 1].  R·M = [0 -1; -1 0], by hand.
    assert_eq!(
        turned.linear(),
        [Rational::ZERO, r(-1, 1), r(-1, 1), Rational::ZERO],
        "the turn was applied on the wrong side of the framing"
    );
    // M·R would be [0 1; 1 0] -- the same entries, opposite signs -- so the
    // fixture really does separate the two orders.
    assert_ne!(
        turned.linear(),
        [Rational::ZERO, Rational::ONE, Rational::ONE, Rational::ZERO]
    );
    // And the determinant is unchanged, which is why a turn needs no
    // invertibility check of its own.
    let [a, b, c, d] = turned.linear();
    let determinant = a
        .checked_mul(d)
        .and_then(|ad| b.checked_mul(c).and_then(|bc| ad.checked_sub(bc)))
        .expect("a determinant");
    assert_eq!(
        determinant,
        r(-1, 1),
        "a mirror's determinant survived a turn"
    );
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
                item: Item::Clip(
                    Clip::new(media, 100, frames(LENGTH))
                        .expect("a clip")
                        .with_mask(Some(
                            Mask::rectangle(r(1, 4), r(1, 4), r(3, 4), r(3, 4)).expect("a mask"),
                        )),
                ),
            },
        )
        .expect("a clip");
    project.forget_history();
    (project, sequence)
}

#[test]
fn a_mask_that_turns_over_its_clip_reaches_the_layer() {
    // The end-to-end case: the lane holds the parameter, the stack hands the
    // renderer a shape, and nothing below the model is told there was a curve.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipMaskMotion {
                track: 0,
                index: 0,
                motion: Some(
                    Motion::new(None, None, None, Some(ramp(Rational::ZERO, Rational::ONE)))
                        .expect("a motion"),
                ),
            },
        )
        .expect("a turn");

    let held = project.sequence(sequence).expect("a sequence");
    let mut corners = std::vec::Vec::new();
    for tick in [0_i64, LENGTH / 2, LENGTH - 1] {
        let stack = held.stack_at(Lane::Picture, at(tick)).expect("a stack");
        let shape = stack[0].mask().expect("a shape").clone();
        corners.push(shape.corners().to_vec());
    }
    assert_eq!(
        corners[0],
        std::vec![
            (r(1, 4), r(1, 4)),
            (r(3, 4), r(1, 4)),
            (r(3, 4), r(3, 4)),
            (r(1, 4), r(3, 4))
        ],
        "at the first frame the parameter is nought, which is no turn at all"
    );
    assert_ne!(corners[0], corners[1], "the shape did not move");
    assert_ne!(corners[1], corners[2], "and it stopped part way");
    // A square turned about its own middle is the same square with its corners
    // renamed, whatever the angle -- so this fixture proves the shape moved
    // and would not prove *where*. The corner-by-corner arithmetic is pinned
    // by `a_shape_turns_about_its_own_centroid` on a shape that is not square.
}

#[test]
fn a_framing_that_turns_over_its_clip_reaches_the_layer() {
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: Some(
                    Transform::scaled(
                        Rational::ONE,
                        Rational::ONE,
                        (Rational::ZERO, Rational::ZERO),
                        Resampling::Bilinear,
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
                motion: Some(
                    Motion::new(None, None, None, Some(ramp(Rational::ZERO, Rational::ONE)))
                        .expect("a motion"),
                ),
            },
        )
        .expect("a turn");

    let held = project.sequence(sequence).expect("a sequence");
    let mut seen = std::vec::Vec::new();
    for tick in [0_i64, 6, 12, 18] {
        let stack = held.stack_at(Lane::Picture, at(tick)).expect("a stack");
        seen.push(stack[0].transform().expect("a framing").linear());
    }
    assert_eq!(
        seen[0],
        [Rational::ONE, Rational::ZERO, Rational::ZERO, Rational::ONE],
        "the first frame is the identity, because the parameter starts at nought"
    );
    // t = 12/24 = 1/2 at the halfway mark, which is the three-four-five turn:
    // cos = 3/5, sin = 4/5. Derived by hand from the parametrisation.
    assert_eq!(
        seen[2],
        [r(3, 5), r(-4, 5), r(4, 5), r(3, 5)],
        "the framing does not carry the turn the curve read"
    );
    for pair in seen.windows(2) {
        assert_ne!(pair[0], pair[1], "the framing stopped turning");
    }
}

#[test]
fn a_motion_that_only_turns_is_a_motion() {
    // The lane counts towards `NoAutomation` like the other three: a motion
    // that turns and does nothing else is a real animation, not an empty one.
    assert!(Motion::new(None, None, None, Some(ramp(Rational::ZERO, Rational::ONE))).is_ok());
    assert_eq!(
        Motion::new(None, None, None, None).expect_err("a refusal"),
        ModelStatus::NoAutomation
    );
}

#[test]
fn a_turn_is_re_based_by_a_cut_like_every_other_lane() {
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipMaskMotion {
                track: 0,
                index: 0,
                motion: Some(
                    Motion::new(None, None, None, Some(ramp(Rational::ZERO, Rational::ONE)))
                        .expect("a motion"),
                ),
            },
        )
        .expect("a turn");
    let item = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item")
        .clone();
    let (head, tail) = item.split(8).expect("two pieces");
    let Item::Clip(tail_clip) = &tail else {
        panic!("a clip");
    };
    let lane = tail_clip
        .mask_motion()
        .expect("an animation")
        .turn()
        .expect("a turn lane");
    assert_eq!(
        lane.value_at(at(0)).expect("a value"),
        r(8, LENGTH),
        "the tail's turn restarted at the cut instead of being re-based"
    );
    assert_eq!(head.join(&tail).expect("one item"), item);
}
