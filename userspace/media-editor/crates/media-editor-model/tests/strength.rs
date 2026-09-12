// SPDX-License-Identifier: GPL-3.0-only
//! Clip-local grade-strength animation.
//!
//! The look digest stays fixed while a zero-to-one curve blends the grade in.
//! Splits and joins rebase the curve with the other clip-local animation lanes.

use media_editor_core::{Digest, Duration, Instant, Rational, Timebase};
use media_editor_model::{
    Clip, Curve, Edit, Interpolation, Item, Keyframe, Lane, MediaAsset, ModelStatus, Project,
    SequenceId, TrackKind,
};

const RATE: Timebase = Timebase::FILM_24;
const LENGTH: i64 = 24;
const IN_POINT: i64 = 100;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a length")
}

fn r(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a rational")
}

fn at(tick: i64) -> Instant {
    Instant::new(tick, RATE)
}

fn look() -> Digest {
    Digest::of(b"a show lut")
}

/// A curve rising from nothing at `from` to full at `to`, straight.
fn ramp(from: i64, to: i64) -> Curve {
    Curve::new(std::vec![
        Keyframe::new(at(from), Rational::ZERO, Interpolation::Linear).expect("a keyframe"),
        Keyframe::new(at(to), Rational::ONE, Interpolation::Linear).expect("a keyframe"),
    ])
    .expect("a curve")
}

fn project(kind: TrackKind) -> (Project, SequenceId) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("room");
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"footage"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    project
        .apply(sequence, Edit::AddTrack { index: 0, kind })
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(
                    Clip::new(media, IN_POINT, frames(LENGTH))
                        .expect("a clip")
                        .with_grade(Some(look())),
                ),
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

fn clip() -> Clip {
    let (project, sequence) = project(TrackKind::Video);
    clip_at(&project, sequence, 0)
}

#[test]
fn a_graded_clip_with_no_curve_is_all_the_way_graded() {
    // One is the neutral multiplier and preserves a fully applied grade.
    let held = clip();
    assert!(held.grade_strength().is_none());
    for offset in [0, 1, LENGTH / 2, LENGTH - 1] {
        assert_eq!(
            held.grade_strength_at(offset).expect("a strength"),
            Rational::ONE,
            "a clip nobody animated read something other than the whole look \
             at offset {offset}"
        );
    }
}

#[test]
fn the_curve_is_read_from_the_clips_own_start() {
    // The whole reason the lane is on the clip. A ramp over the first half of
    // the clip reads nought at its first frame and one at the halfway mark,
    // wherever on the timeline the clip happens to sit.
    let held = clip()
        .with_grade_strength(Some(ramp(0, LENGTH / 2)))
        .expect("a strength");
    assert_eq!(
        held.grade_strength_at(0).expect("a strength"),
        Rational::ZERO
    );
    assert_eq!(
        held.grade_strength_at(3).expect("a strength"),
        r(1, 4),
        "three ticks into a twelve-tick ramp is a quarter of the way on"
    );
    assert_eq!(
        held.grade_strength_at(LENGTH / 2).expect("a strength"),
        Rational::ONE
    );
    assert_eq!(
        held.grade_strength_at(LENGTH - 1).expect("a strength"),
        Rational::ONE,
        "a curve holds past its last keyframe"
    );
}

#[test]
fn an_overshooting_ease_is_clamped_at_the_read() {
    // An ease's verticals are deliberately unclamped, because an overshoot is
    // a useful thing for a curve to do. A strength past one is not: it asks
    // for the picture on the far side of a table that was never sampled
    // there, which is arithmetic rather than a grade. Clamped rather than
    // refused, because that is what a track's automation and a clip's own
    // opacity both do — one rule, not three that have to agree.
    //
    // A guard is only checked by an input that reaches it, so this fixture
    // deliberately overshoots rather than merely being legal at its ends.
    let curve = Curve::new(std::vec![
        Keyframe::new(
            at(0),
            Rational::ZERO,
            // Both handles well past the top, so the curve between two legal
            // strengths rises above one on its way.
            Interpolation::Ease {
                out_x: r(1, 3),
                out_y: r(5, 2),
                in_x: r(2, 3),
                in_y: r(5, 2),
            },
        )
        .expect("a keyframe"),
        Keyframe::new(at(LENGTH - 1), Rational::ONE, Interpolation::Linear).expect("a keyframe"),
    ])
    .expect("a curve");
    let held = clip()
        .with_grade_strength(Some(curve.clone()))
        .expect("a strength");

    let mut overshot = false;
    for offset in 0..LENGTH {
        let raw = curve.value_at(at(offset)).expect("a value");
        overshot |= raw > Rational::ONE;
        let read = held.grade_strength_at(offset).expect("a strength");
        assert!(
            read >= Rational::ZERO && read <= Rational::ONE,
            "the read let {raw:?} through at offset {offset}"
        );
    }
    assert!(
        overshot,
        "the fixture never leaves the range, so the clamp was never reached \
         and nothing above proves anything"
    );
}

#[test]
fn a_strength_needs_a_grade_to_be_the_strength_of() {
    // The same invariant a motion has against a transform and a mask
    // animation has against a mask. An animation of nothing is a value no
    // sequence of edits could give meaning to, and every later reader would be
    // entitled to assume it meant something.
    let ungraded = clip().with_grade(None);
    assert_eq!(
        ungraded
            .with_grade_strength(Some(ramp(0, LENGTH)))
            .expect_err("a refusal"),
        ModelStatus::NoGradeToAnimate
    );
    // And a curve counted another way than the clip is, which would read the
    // animation at the wrong frames for the whole clip, silently.
    let other = Curve::new(std::vec![
        Keyframe::new(
            Instant::new(0, Timebase::NTSC_30),
            Rational::ZERO,
            Interpolation::Linear,
        )
        .expect("a keyframe"),
    ])
    .expect("a curve");
    assert_eq!(
        clip()
            .with_grade_strength(Some(other))
            .expect_err("a refusal"),
        ModelStatus::WrongTimebase
    );
}

#[test]
fn taking_the_grade_off_an_animated_clip_is_refused() {
    // By the same door the mask closes. Dropping the curve here instead would
    // discard work the caller did not name — and make the inverse a lie,
    // because undoing would put the look back with its arrival already gone.
    let (mut project, sequence) = project(TrackKind::Video);
    project
        .apply(
            sequence,
            Edit::SetClipGradeStrength {
                track: 0,
                index: 0,
                strength: Some(ramp(0, LENGTH)),
            },
        )
        .expect("a strength");
    assert_eq!(
        project
            .apply(
                sequence,
                Edit::SetClipGrade {
                    track: 0,
                    index: 0,
                    grade: None,
                },
            )
            .expect_err("a refusal"),
        ModelStatus::NoGradeToAnimate
    );
}

#[test]
fn a_cut_re_bases_the_arrival_onto_the_tail() {
    // Carried across unchanged the look would restart arriving at the cut,
    // which is worse than not animating at all: the tail would still move,
    // just from the wrong place.
    let head_length = 8;
    let animated = clip()
        .with_grade_strength(Some(ramp(0, LENGTH)))
        .expect("a strength");
    let (first, second) = Item::Clip(animated).split(head_length).expect("two pieces");
    let (Item::Clip(head), Item::Clip(tail)) = (first, second) else {
        panic!("two clips");
    };
    for offset in 0..head_length {
        assert_eq!(
            head.grade_strength_at(offset).expect("a strength"),
            r(offset, LENGTH),
            "the head is the original, unchanged"
        );
    }
    for offset in 0..(LENGTH - head_length) {
        assert_eq!(
            tail.grade_strength_at(offset).expect("a strength"),
            r(offset + head_length, LENGTH),
            "the tail restarted its arrival at the cut"
        );
    }
}

#[test]
fn a_join_is_the_exact_inverse_of_a_split() {
    // Which is the property that says the re-basing above is right rather than
    // merely plausible.
    let whole = Item::Clip(
        clip()
            .with_grade_strength(Some(ramp(0, LENGTH)))
            .expect("a strength"),
    );
    let (head, tail) = whole.split(8).expect("two pieces");
    assert_eq!(head.join(&tail).expect("one item"), whole);
}

#[test]
fn two_clips_whose_arrivals_do_not_line_up_do_not_join() {
    // Three arms, all three exercised, because a test named for two animated
    // clips that only animates one reaches exactly one of them — which this
    // project has recorded happening once already.
    let plain = Item::Clip(clip());
    let (plain_head, plain_tail) = plain.split(8).expect("two pieces");
    assert!(
        plain_head.continues_into(&plain_tail),
        "neither animated is the arm that must say yes"
    );

    let animated = Item::Clip(
        clip()
            .with_grade_strength(Some(ramp(0, LENGTH)))
            .expect("a strength"),
    );
    let (head, tail) = animated.split(8).expect("two pieces");

    // One side animated: the pair is two shots, not one cut in two.
    assert!(
        !head.continues_into(&plain_tail),
        "half an animation survived a join that had no business happening"
    );
    assert!(!plain_head.continues_into(&tail));

    // Both animated and disagreeing: also two shots. The tail's curve is the
    // head's re-based, so a *differently* re-based one must be refused.
    let Item::Clip(tail_clip) = &tail else {
        panic!("a clip");
    };
    let elsewhere = Item::Clip(
        tail_clip
            .with_grade_strength(Some(ramp(0, LENGTH)))
            .expect("a strength"),
    );
    assert!(
        !head.continues_into(&elsewhere),
        "two arrivals that do not line up were fused and one of them dropped"
    );
}

#[test]
fn the_edit_applies_and_its_inverse_gives_back_the_curve_it_replaced() {
    let (mut project, sequence) = project(TrackKind::Video);
    let first = ramp(0, LENGTH);
    let second = ramp(4, LENGTH - 4);
    project
        .apply(
            sequence,
            Edit::SetClipGradeStrength {
                track: 0,
                index: 0,
                strength: Some(first.clone()),
            },
        )
        .expect("a strength");
    assert_eq!(
        clip_at(&project, sequence, 0).grade_strength(),
        Some(&first),
        "the edit reported success and nothing was set"
    );
    project
        .apply(
            sequence,
            Edit::SetClipGradeStrength {
                track: 0,
                index: 0,
                strength: Some(second.clone()),
            },
        )
        .expect("a strength");
    assert_eq!(
        clip_at(&project, sequence, 0).grade_strength(),
        Some(&second)
    );
    project.undo(sequence).expect("undone");
    assert_eq!(
        clip_at(&project, sequence, 0).grade_strength(),
        Some(&first),
        "undoing threw away the shape somebody drew instead of putting it back"
    );
    project.undo(sequence).expect("undone");
    assert_eq!(clip_at(&project, sequence, 0).grade_strength(), None);
}

#[test]
fn a_gap_has_no_look_to_bring_on() {
    let (mut project, sequence) = project(TrackKind::Video);
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 1,
                item: Item::gap(frames(6)).expect("a gap"),
            },
        )
        .expect("a gap");
    assert_eq!(
        project
            .apply(
                sequence,
                Edit::SetClipGradeStrength {
                    track: 0,
                    index: 1,
                    strength: Some(ramp(0, 6)),
                },
            )
            .expect_err("a refusal"),
        ModelStatus::NotAClip
    );
}

#[test]
fn the_layer_carries_the_resolved_strength() {
    // Resolved in the stack rather than handed out as a curve, exactly as the
    // framing and the shape are. A renderer that had to be told about curves
    // would need a clock, and a node that depends on a clock is a node whose
    // cache key is a lie.
    let (mut project, sequence) = project(TrackKind::Video);
    project
        .apply(
            sequence,
            Edit::SetClipGradeStrength {
                track: 0,
                index: 0,
                strength: Some(ramp(0, LENGTH)),
            },
        )
        .expect("a strength");
    let held = project.sequence(sequence).expect("a sequence");
    let mut seen = std::vec::Vec::new();
    for tick in [0_i64, 6, 12, 18] {
        let stack = held.stack_at(Lane::Picture, at(tick)).expect("a stack");
        let graded = stack[0].grade().expect("a look on the layer");
        assert_eq!(graded.look(), look(), "the layer lost which look it is");
        assert_eq!(
            graded.strength(),
            r(tick, LENGTH),
            "the layer read the arrival at the wrong frame"
        );
        seen.push(graded.strength());
    }
    assert!(
        seen.windows(2).all(|pair| pair[0] < pair[1]),
        "the fixture does not vary along the axis under test"
    );
}

#[test]
fn an_ungraded_layer_has_no_strength_rather_than_a_neutral_one() {
    // Which is the whole reason the layer carries a pair. A strength means
    // nothing without a look to be the strength of, and a neutral one on an
    // ungraded layer would be a value every reader has to know to ignore.
    let (mut project, sequence) = project(TrackKind::Video);
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: None,
            },
        )
        .expect("no grade");
    let stack = project
        .sequence(sequence)
        .expect("a sequence")
        .stack_at(Lane::Picture, at(0))
        .expect("a stack");
    assert_eq!(stack[0].grade(), None);
}
