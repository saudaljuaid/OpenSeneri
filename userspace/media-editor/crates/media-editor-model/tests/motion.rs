// SPDX-License-Identifier: GPL-3.0-only
//! Clip-local framing animation.
//!
//! Splits rebase animation onto the tail, joins restore the original curve,
//! and scale remains positive throughout interpolation.

use media_editor_core::{Digest, Duration, Instant, Rational, Timebase};
use media_editor_model::{
    Clip, Curve, Edit, Interpolation, Item, Keyframe, Lane, MediaAsset, ModelStatus, Motion,
    Project, Resampling, SequenceId, TrackKind, Transform, Turn,
};

const RATE: Timebase = Timebase::FILM_24;

/// How long the clip in the fixture is.
const LENGTH: i64 = 48;

fn r(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a rational")
}

/// The base framing: half size, centred, area-filtered.
fn half() -> Transform {
    Transform::scaled(
        r(1, 2),
        r(1, 2),
        (Rational::ZERO, Rational::ZERO),
        Resampling::Area,
    )
    .expect("a transform")
}

fn at(ticks: i64) -> Instant {
    Instant::new(ticks, RATE)
}

/// A curve running straight from `from` at tick nought to `to` at `LENGTH`.
fn ramp(from: Rational, to: Rational) -> Curve {
    Curve::new(vec![
        Keyframe::new(at(0), from, Interpolation::Linear).expect("a keyframe"),
        Keyframe::new(at(LENGTH), to, Interpolation::Linear).expect("a keyframe"),
    ])
    .expect("a curve")
}

/// A push in from the size the base says to twice it, over the whole clip.
fn push_in() -> Motion {
    Motion::new(Some(ramp(Rational::ONE, r(2, 1))), None, None, None).expect("a motion")
}

/// A project with one track and, at `index`, a clip of `LENGTH` ticks.
///
/// `leading` gaps go before it, which is how a clip comes to start somewhere
/// other than at the programme's own nought — the case the whole "measured
/// from the clip" decision is about.
fn project(leading: i64) -> (Project, SequenceId) {
    let mut project = Project::new();
    let media = project
        .add_media(
            MediaAsset::new(
                Digest::of(b"animated"),
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
    let mut index = 0;
    if leading > 0 {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index: 0,
                    item: Item::gap(Duration::new(leading, RATE).expect("a length"))
                        .expect("a gap"),
                },
            )
            .expect("a gap");
        index = 1;
    }
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index,
                item: Item::Clip(
                    Clip::new(media, 0, Duration::new(LENGTH, RATE).expect("a length"))
                        .expect("a clip"),
                ),
            },
        )
        .expect("a clip");
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index,
                transform: Some(half()),
            },
        )
        .expect("a framing");
    project.forget_history();
    (project, sequence)
}

/// The clip at `index` on the fixture's only track.
fn clip_at(project: &Project, sequence: SequenceId, index: usize) -> Clip {
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
    clip.clone()
}

/// The framing the stack hands the renderer at a programme instant.
fn framing(project: &Project, sequence: SequenceId, ticks: i64) -> Option<Transform> {
    project
        .sequence(sequence)
        .expect("a sequence")
        .stack_at(Lane::Picture, at(ticks))
        .expect("a stack")[0]
        .transform()
}

/// A uniform scale, as the transform the base becomes under one.
fn scaled_by(factor: Rational) -> Transform {
    Transform::scaled(
        r(1, 2).checked_mul(factor).expect("a size"),
        r(1, 2).checked_mul(factor).expect("a size"),
        (Rational::ZERO, Rational::ZERO),
        Resampling::Area,
    )
    .expect("a transform")
}

#[test]
fn a_scale_that_pushes_in_reads_its_way_between_two_keyframes() {
    let (mut project, sequence) = project(0);
    project
        .apply(
            sequence,
            Edit::SetClipMotion {
                track: 0,
                index: 0,
                motion: Some(push_in()),
            },
        )
        .expect("a motion");
    // A ramp from one to two over forty-eight ticks: a quarter of the way in
    // it reads five quarters, and the base's half becomes five eighths.
    assert_eq!(
        framing(&project, sequence, 0),
        Some(scaled_by(Rational::ONE))
    );
    assert_eq!(framing(&project, sequence, 12), Some(scaled_by(r(5, 4))));
    assert_eq!(framing(&project, sequence, 24), Some(scaled_by(r(3, 2))));
    assert_eq!(framing(&project, sequence, 47), Some(scaled_by(r(95, 48))));
}

#[test]
fn a_motion_is_measured_from_the_clip_and_not_from_the_programme() {
    // The same clip, the same motion, twelve ticks further down the timeline.
    // If the animation were measured from the programme it would already be a
    // quarter done when the clip started, and the push-in would be clipped.
    let (mut project, sequence) = project(12);
    project
        .apply(
            sequence,
            Edit::SetClipMotion {
                track: 0,
                index: 1,
                motion: Some(push_in()),
            },
        )
        .expect("a motion");
    assert_eq!(
        framing(&project, sequence, 12),
        Some(scaled_by(Rational::ONE)),
        "the clip's first frame is the animation's first frame"
    );
    assert_eq!(framing(&project, sequence, 36), Some(scaled_by(r(3, 2))));
}

#[test]
fn a_clip_moved_down_the_timeline_animates_the_same() {
    let mut framings = Vec::new();
    for leading in [0, 12] {
        let (mut project, sequence) = project(leading);
        let index = usize::from(leading > 0);
        project
            .apply(
                sequence,
                Edit::SetClipMotion {
                    track: 0,
                    index,
                    motion: Some(push_in()),
                },
            )
            .expect("a motion");
        framings.push(
            (0..LENGTH)
                .map(|tick| framing(&project, sequence, leading + tick))
                .collect::<Vec<_>>(),
        );
    }
    assert_eq!(
        framings[0], framings[1],
        "moving a shot is not re-animating it"
    );
}

#[test]
fn a_lane_with_no_curve_reads_its_neutral() {
    // One for the scale, which multiplies; nought for the moves, which add.
    // Written as an assertion rather than left to the type, because a neutral
    // read wrong is a whole clip in the wrong place and nothing in the code
    // that says so.
    let motion =
        Motion::new(None, Some(ramp(Rational::ZERO, r(1, 4))), None, None).expect("a motion");
    assert_eq!(
        motion.at(at(0)).expect("a reading"),
        (Rational::ONE, Rational::ZERO, Rational::ZERO, Turn::NONE)
    );
    assert_eq!(
        motion.at(at(LENGTH)).expect("a reading"),
        (Rational::ONE, r(1, 4), Rational::ZERO, Turn::NONE)
    );
}

#[test]
fn a_scale_multiplies_the_framing_and_a_move_adds_to_it() {
    // A mirror stays a mirror while it pushes in, which is the whole reason a
    // motion is a scale and two moves rather than four curves over the linear
    // part: the base holds the shape and this changes the size and the place.
    let mirrored = Transform::new(
        [r(-1, 1), Rational::ZERO, Rational::ZERO, Rational::ONE],
        (r(1, 10), Rational::ZERO),
        Resampling::Bilinear,
    )
    .expect("a transform");
    let moved = mirrored
        .moved_by(r(3, 2), r(1, 5), r(-1, 4), Turn::NONE)
        .expect("a framing");
    assert_eq!(
        moved.linear(),
        [r(-3, 2), Rational::ZERO, Rational::ZERO, r(3, 2)],
        "still a mirror, half again as big"
    );
    assert_eq!(moved.offset(), (r(3, 10), r(-1, 4)));
    assert_eq!(
        moved.resampling(),
        Resampling::Bilinear,
        "the filter is the base's, not the motion's"
    );
}

#[test]
fn an_ease_that_dips_below_nothing_is_refused_when_it_is_read() {
    // Both keyframes are positive, so `Motion::new` has nothing to refuse.
    // The overshoot happens between them: an ease's verticals are deliberately
    // unclamped, and an outgoing handle at minus four sends the value to
    // roughly minus three eighths halfway along. That is a guard reached only
    // by an input that reaches it, which is why the ease here is one that
    // actually gets there rather than one that merely could.
    let dipping = Curve::new(vec![
        Keyframe::new(
            at(0),
            Rational::ONE,
            Interpolation::Ease {
                out_x: r(1, 3),
                out_y: r(-4, 1),
                in_x: r(2, 3),
                in_y: Rational::ZERO,
            },
        )
        .expect("a keyframe"),
        Keyframe::new(at(LENGTH), r(2, 1), Interpolation::Linear).expect("a keyframe"),
    ])
    .expect("a curve");
    let motion = Motion::new(Some(dipping), None, None, None).expect("a motion");
    let (scale, _, _, _) = motion.at(at(LENGTH / 2)).expect("a reading");
    assert!(
        !scale.is_positive(),
        "the ease must actually reach the guard, and it read {scale:?}"
    );
    assert_eq!(
        half().moved_by(scale, Rational::ZERO, Rational::ZERO, Turn::NONE),
        Err(ModelStatus::ScaleNotPositive)
    );

    let (mut project, sequence) = project(0);
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
    // And the stack refuses rather than handing the renderer a framing that
    // flattens the picture onto a point.
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .stack_at(Lane::Picture, at(LENGTH / 2)),
        Err(ModelStatus::ScaleNotPositive)
    );
}

#[test]
fn a_scale_keyframe_at_nothing_or_below_is_refused() {
    for value in [Rational::ZERO, r(-1, 1)] {
        assert_eq!(
            Motion::new(Some(ramp(Rational::ONE, value)), None, None, None),
            Err(ModelStatus::ScaleNotPositive),
            "a scale keyframe of {value:?}"
        );
    }
    // A move may be nought or negative — that is left and up.
    assert!(Motion::new(None, Some(ramp(Rational::ZERO, r(-1, 4))), None, None).is_ok());
}

#[test]
fn a_motion_that_animates_nothing_is_refused() {
    assert_eq!(
        Motion::new(None, None, None, None),
        Err(ModelStatus::NoAutomation)
    );
}

#[test]
fn a_motion_needs_a_framing_to_animate() {
    let (mut project, sequence) = project(0);
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: None,
            },
        )
        .expect("no framing");
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipMotion {
                track: 0,
                index: 0,
                motion: Some(push_in()),
            },
        ),
        Err(ModelStatus::NoTransformToAnimate)
    );
}

#[test]
fn the_framing_cannot_be_taken_off_an_animated_clip() {
    // The other half of the same invariant, and the one that is easy to leave
    // out: setting the motion is guarded, so unsetting the transform must be
    // too, or the project reaches the state the guard exists to prevent by
    // the other door.
    let (mut project, sequence) = project(0);
    project
        .apply(
            sequence,
            Edit::SetClipMotion {
                track: 0,
                index: 0,
                motion: Some(push_in()),
            },
        )
        .expect("a motion");
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: None,
            },
        ),
        Err(ModelStatus::NoTransformToAnimate)
    );
    assert_eq!(
        clip_at(&project, sequence, 0).transform(),
        Some(half()),
        "and nothing was changed on the way to the refusal"
    );
}

#[test]
fn a_gap_cannot_be_animated() {
    let (mut project, sequence) = project(12);
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipMotion {
                track: 0,
                index: 0,
                motion: Some(push_in()),
            },
        ),
        Err(ModelStatus::NotAClip)
    );
}

#[test]
fn a_motion_counted_another_way_is_refused_when_it_is_set() {
    // Refused here rather than at the first frame that reads it. `value_at`
    // would refuse either way; the difference is whether the editor is told
    // when they set it or the render is.
    let elsewhere = Curve::new(vec![
        Keyframe::new(
            Instant::new(0, Timebase::PAL_25),
            Rational::ONE,
            Interpolation::Linear,
        )
        .expect("a keyframe"),
        Keyframe::new(
            Instant::new(50, Timebase::PAL_25),
            r(2, 1),
            Interpolation::Linear,
        )
        .expect("a keyframe"),
    ])
    .expect("a curve");
    let (mut project, sequence) = project(0);
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipMotion {
                track: 0,
                index: 0,
                motion: Some(Motion::new(Some(elsewhere), None, None, None).expect("a motion")),
            },
        ),
        Err(ModelStatus::WrongTimebase)
    );
}

#[test]
fn a_motion_is_set_by_an_edit_and_undone_by_its_inverse() {
    let (mut project, sequence) = project(0);
    project
        .apply(
            sequence,
            Edit::SetClipMotion {
                track: 0,
                index: 0,
                motion: Some(push_in()),
            },
        )
        .expect("a motion");
    assert_eq!(clip_at(&project, sequence, 0).motion(), Some(&push_in()));
    project.undo(sequence).expect("an undo");
    assert_eq!(clip_at(&project, sequence, 0).motion(), None);
    project.redo(sequence).expect("a redo");
    assert_eq!(clip_at(&project, sequence, 0).motion(), Some(&push_in()));
}

#[test]
fn a_split_tail_reads_the_animation_it_was_in_the_middle_of() {
    // The property the re-basing exists for. Cut at tick twenty-four, and the
    // tail's *first* frame must read what the whole clip read at twenty-four
    // — three halves — rather than starting the push-in again from one.
    let (mut project, sequence) = project(0);
    project
        .apply(
            sequence,
            Edit::SetClipMotion {
                track: 0,
                index: 0,
                motion: Some(push_in()),
            },
        )
        .expect("a motion");
    let before: Vec<_> = (0..LENGTH)
        .map(|tick| framing(&project, sequence, tick))
        .collect();
    project
        .apply(
            sequence,
            Edit::SplitItem {
                track: 0,
                index: 0,
                offset: 24,
            },
        )
        .expect("a cut");
    let after: Vec<_> = (0..LENGTH)
        .map(|tick| framing(&project, sequence, tick))
        .collect();
    assert_eq!(
        before, after,
        "a cut through an animated clip changes nothing about what it shows"
    );
    assert_eq!(after[24], Some(scaled_by(r(3, 2))));
}

#[test]
fn a_split_re_bases_the_tail_and_a_join_puts_it_back() {
    let (mut project, sequence) = project(0);
    project
        .apply(
            sequence,
            Edit::SetClipMotion {
                track: 0,
                index: 0,
                motion: Some(push_in()),
            },
        )
        .expect("a motion");
    project
        .apply(
            sequence,
            Edit::SplitItem {
                track: 0,
                index: 0,
                offset: 24,
            },
        )
        .expect("a cut");
    let tail = clip_at(&project, sequence, 1);
    let keyframes: Vec<i64> = tail
        .motion()
        .expect("an animation")
        .scale()
        .expect("a scale lane")
        .keyframes()
        .iter()
        .map(|keyframe| keyframe.at().ticks())
        .collect();
    assert_eq!(
        keyframes,
        vec![-24, 24],
        "the keyframe before the cut goes negative rather than being dropped"
    );
    project
        .apply(sequence, Edit::JoinItems { track: 0, index: 0 })
        .expect("a join");
    assert_eq!(
        clip_at(&project, sequence, 0).motion(),
        Some(&push_in()),
        "join is the exact inverse of split, animation included"
    );
}

#[test]
fn clips_whose_animations_do_not_line_up_do_not_join() {
    // Three ways two halves of one cut can disagree about their animation, and
    // all three must be refused rather than reconciled -- joining would keep
    // the first's move and discard the second's without saying so.
    //
    // Written as three cases because the check is a `match` and the first
    // version of this test only ever reached one of its arms: the fixture
    // animated the tail and left the head alone, so a control that made two
    // *present* animations always agree changed nothing and the test passed.
    for (head, tail) in [
        (None, Some(push_in())),
        (Some(push_in()), None),
        // Both animated, and both from the clip's own start -- which is
        // exactly what a split does *not* produce, because the tail's is
        // re-based. Two shots that move differently.
        (Some(push_in()), Some(push_in())),
    ] {
        let (mut project, sequence) = project(0);
        project
            .apply(
                sequence,
                Edit::SplitItem {
                    track: 0,
                    index: 0,
                    offset: 24,
                },
            )
            .expect("a cut");
        for (index, motion) in [(0, head.clone()), (1, tail.clone())] {
            project
                .apply(
                    sequence,
                    Edit::SetClipMotion {
                        track: 0,
                        index,
                        motion,
                    },
                )
                .expect("a motion");
        }
        assert_eq!(
            project.apply(sequence, Edit::JoinItems { track: 0, index: 0 }),
            Err(ModelStatus::ItemsNotContiguous),
            "head {:?}, tail {:?}",
            head.is_some(),
            tail.is_some()
        );
    }
}

#[test]
fn a_motion_survives_a_slip_and_a_trim() {
    // The fault that was found three times over — a builder that rebuilds from
    // `Clip::new` drops everything it was not told about — asked of the field
    // added last.
    let (mut project, sequence) = project(0);
    project
        .apply(
            sequence,
            Edit::SetClipMotion {
                track: 0,
                index: 0,
                motion: Some(push_in()),
            },
        )
        .expect("a motion");
    project
        .apply(
            sequence,
            Edit::SetClipSource {
                track: 0,
                index: 0,
                source_start: 100,
            },
        )
        .expect("a slip");
    assert_eq!(clip_at(&project, sequence, 0).motion(), Some(&push_in()));
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
        clip_at(&project, sequence, 0).motion(),
        Some(&push_in()),
        "a shorter clip runs less of the same move, not none of it"
    );
}

#[test]
fn a_shifted_curve_reads_the_same_at_the_shifted_instant() {
    // The arithmetic underneath the re-basing, on its own: a curve moved by
    // `by` reads at `t + by` what it used to read at `t`. Everywhere, not
    // only between the keyframes — which is what makes a negative keyframe
    // meaningful rather than merely tolerated.
    let curve = ramp(Rational::ONE, r(2, 1));
    let shifted = curve.shifted(-24).expect("a shift");
    for tick in -24..LENGTH + 24 {
        assert_eq!(
            curve.value_at(at(tick)).expect("a value"),
            shifted.value_at(at(tick - 24)).expect("a value"),
            "at tick {tick}"
        );
    }
}
