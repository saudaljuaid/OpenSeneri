// SPDX-License-Identifier: GPL-3.0-only
//! Clip-local opacity animation.
//!
//! Opacity curves combine with simple clip fades and apply equally to recorded
//! media and titles.

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
                item: Item::Clip(Clip::new(media, IN_POINT, frames(LENGTH)).expect("a clip")),
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
fn a_clip_with_no_curve_is_fully_opaque() {
    // One is the neutral value for something that multiplies, and `None` is
    // not a curve holding one -- it is a clip with no animation at all, which
    // is what lets an animation be switched off and back on without losing the
    // shape somebody drew.
    let plain = clip();
    assert!(plain.opacity().is_none());
    for offset in [0, 1, LENGTH - 1] {
        assert_eq!(plain.opacity_at(offset).expect("a fraction"), Rational::ONE);
    }
}

#[test]
fn a_curve_is_read_from_the_clips_own_start() {
    // Twelve frames from nothing to full, so six frames in is a half. If it
    // were read from the *timeline* it would give the same answer only for a
    // clip that happens to begin at nought.
    let rising = clip().with_opacity(Some(ramp(0, 12))).expect("a rise");
    assert_eq!(rising.opacity_at(0).expect("a fraction"), Rational::ZERO);
    assert_eq!(rising.opacity_at(6).expect("a fraction"), r(1, 2));
    assert_eq!(rising.opacity_at(12).expect("a fraction"), Rational::ONE);
    assert_eq!(
        rising.opacity_at(LENGTH - 1).expect("a fraction"),
        Rational::ONE,
        "held past the last keyframe rather than extrapolated"
    );
}

#[test]
fn a_curve_that_overshoots_is_clamped_rather_than_refused() {
    // An ease between two legal keyframes can overshoot on the way,
    // deliberately -- the verticals are unclamped because an overshoot is a
    // useful thing for a curve to do. A layer at more than full coverage is a
    // frame the compositor refuses, so the *read* clamps, exactly as a track's
    // automation does. One rule for both rather than two that have to agree.
    let overshooting = Curve::new(std::vec![
        Keyframe::new(
            at(0),
            Rational::ZERO,
            Interpolation::Ease {
                out_x: r(1, 4),
                out_y: r(3, 1),
                in_x: r(3, 4),
                in_y: r(3, 1),
            },
        )
        .expect("a keyframe"),
        Keyframe::new(at(12), Rational::ONE, Interpolation::Linear).expect("a keyframe"),
    ])
    .expect("a curve");
    let held = clip().with_opacity(Some(overshooting)).expect("an ease");
    for offset in 0..=12 {
        let read = held.opacity_at(offset).expect("a fraction");
        assert!(
            read >= Rational::ZERO && read <= Rational::ONE,
            "at {offset} the curve read outside the coverage it is"
        );
    }
    assert_eq!(
        held.opacity_at(6).expect("a fraction"),
        Rational::ONE,
        "and the middle of that ease is well past full, so it is held at full"
    );
}

#[test]
fn a_curve_counted_another_way_is_refused() {
    // A clip and its animation that disagree about what a tick is would read
    // the curve at the wrong frames, silently, for the whole clip.
    let other = Timebase::PAL_25;
    let elsewhere = Curve::new(std::vec![
        Keyframe::new(
            Instant::new(0, other),
            Rational::ZERO,
            Interpolation::Linear
        )
        .expect("a keyframe"),
        Keyframe::new(
            Instant::new(12, other),
            Rational::ONE,
            Interpolation::Linear
        )
        .expect("a keyframe"),
    ])
    .expect("a curve");
    assert_eq!(
        clip().with_opacity(Some(elsewhere)),
        Err(ModelStatus::WrongTimebase)
    );
}

#[test]
fn the_track_the_clip_and_the_fade_all_multiply() {
    // Three things can decide what is on screen at once, and any one of them
    // replacing the others would throw away a decision somebody made. The
    // stack is where they meet.
    let (mut project, sequence) = project(TrackKind::Video);
    for edit in [
        Edit::SetTrackOpacity {
            track: 0,
            opacity: Some(
                Curve::new(std::vec![
                    Keyframe::new(at(0), r(1, 2), Interpolation::Hold).expect("a keyframe"),
                ])
                .expect("a curve"),
            ),
        },
        Edit::SetClipOpacity {
            track: 0,
            index: 0,
            opacity: Some(ramp(0, 12)),
        },
    ] {
        project.apply(sequence, edit).expect("an edit");
    }
    let layer = |tick| {
        project
            .sequence(sequence)
            .expect("a sequence")
            .stack_at(Lane::Picture, at(tick))
            .expect("a stack")[0]
            .opacity()
    };
    // The track holds a half throughout; the clip rises 0, 1/2, 1 over twelve.
    assert_eq!(layer(0), Rational::ZERO);
    assert_eq!(layer(6), r(1, 4), "a half of a half");
    assert_eq!(layer(12), r(1, 2), "a half of all of it");
}

#[test]
fn a_clips_curve_and_its_fade_are_different_things() {
    // The fade reaches the renderer separately from the opacity -- one is what
    // the clip is doing to itself and the other is what the track and any
    // transition are doing -- so a test can tell them apart, and this one does.
    let (mut project, sequence) = project(TrackKind::Video);
    for edit in [
        Edit::SetClipFades {
            track: 0,
            index: 0,
            fade_in: frames(4),
            fade_out: frames(0),
        },
        Edit::SetClipOpacity {
            track: 0,
            index: 0,
            opacity: Some(ramp(0, 12)),
        },
    ] {
        project.apply(sequence, edit).expect("an edit");
    }
    // Sampled at two, where *both* are part way through -- at six the fade
    // would be finished, and a fade of one cannot be told apart from a fade
    // that was never carried. Eighth time a fixture has had to be moved off a
    // value that does not vary along the axis under test.
    let stack = project
        .sequence(sequence)
        .expect("a sequence")
        .stack_at(Lane::Picture, at(2))
        .expect("a stack");
    assert_eq!(stack[0].opacity(), r(1, 6), "the curve, at two of twelve");
    assert_eq!(stack[0].fade(), r(1, 2), "the fade, at two of four");
    assert_ne!(
        stack[0].opacity(),
        r(1, 12),
        "and they are carried apart rather than multiplied together on the way"
    );
}

#[test]
fn a_cut_re_bases_the_tail_of_an_animation() {
    // The same fault the motion had and for the same reason: a curve measured
    // from its clip's start, carried unchanged onto a tail whose start is
    // later, restarts the animation at the cut. The tail would still animate,
    // just from the wrong place, which is harder to see than not animating.
    let (mut project, sequence) = project(TrackKind::Video);
    project
        .apply(
            sequence,
            Edit::SetClipOpacity {
                track: 0,
                index: 0,
                opacity: Some(ramp(0, 12)),
            },
        )
        .expect("a rise");
    let whole: std::vec::Vec<Rational> = (0..LENGTH)
        .map(|offset| {
            clip_at(&project, sequence, 0)
                .opacity_at(offset)
                .expect("a fraction")
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
            tail.opacity_at(offset).expect("a fraction"),
            whole[usize::try_from(offset + 8).expect("an index")],
            "the tail at {offset} shows what the whole showed at {}",
            offset + 8
        );
    }
}

#[test]
fn two_halves_of_an_animated_clip_join_back_into_one() {
    // Join is the exact inverse of split, which is what says the re-basing
    // above is right rather than merely plausible.
    let (mut project, sequence) = project(TrackKind::Video);
    for edit in [
        Edit::SetClipOpacity {
            track: 0,
            index: 0,
            opacity: Some(ramp(0, 12)),
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
    assert_eq!(whole.opacity(), Some(&ramp(0, 12)));
}

#[test]
fn two_clips_whose_animations_do_not_line_up_do_not_join() {
    // The negative the test above needs beside it: a join that ignored the
    // curves would fuse two shots and keep one animation, silently.
    let first = clip().with_opacity(Some(ramp(0, 12))).expect("a rise");
    let second = clip()
        .with_source(IN_POINT + LENGTH)
        .expect("the frames after")
        .with_opacity(Some(ramp(0, 12)))
        .expect("a rise that restarts");
    assert!(
        !Item::Clip(first.clone()).continues_into(&Item::Clip(second)),
        "an animation that restarts at the cut is not one animation"
    );
    // And one that is animated beside one that is not.
    let plain = clip().with_source(IN_POINT + LENGTH).expect("the next");
    assert!(!Item::Clip(first).continues_into(&Item::Clip(plain)));
}

#[test]
fn turning_an_animation_off_and_on_keeps_the_shape() {
    // Which is what `None` being a different thing from a curve holding one
    // buys: an editor switching an animation off has not thrown it away.
    let (mut project, sequence) = project(TrackKind::Video);
    let drawn = ramp(0, 12);
    project
        .apply(
            sequence,
            Edit::SetClipOpacity {
                track: 0,
                index: 0,
                opacity: Some(drawn.clone()),
            },
        )
        .expect("a rise");
    project
        .apply(
            sequence,
            Edit::SetClipOpacity {
                track: 0,
                index: 0,
                opacity: None,
            },
        )
        .expect("off");
    assert!(clip_at(&project, sequence, 0).opacity().is_none());
    project.undo(sequence).expect("an undo");
    assert_eq!(
        clip_at(&project, sequence, 0).opacity(),
        Some(&drawn),
        "the shape somebody drew, not a curve holding one"
    );
}

#[test]
fn sound_has_no_opacity() {
    // A sound clip's loudness is its track's fader and its own fade, in
    // decibels. An opacity is a coverage, and the two are not one quantity
    // wearing two names -- one multiplies light and the other is a logarithm
    // of amplitude.
    let (mut project, sequence) = project(TrackKind::Audio);
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipOpacity {
                track: 0,
                index: 0,
                opacity: Some(ramp(0, 12)),
            },
        ),
        Err(ModelStatus::OpacityOnSound)
    );
}

#[test]
fn a_gap_has_nothing_to_reveal() {
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
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::gap(frames(LENGTH)).expect("a gap"),
            },
        )
        .expect("a gap");
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipOpacity {
                track: 0,
                index: 0,
                opacity: Some(ramp(0, 12)),
            },
        ),
        Err(ModelStatus::NotAClip)
    );
}

#[test]
fn a_title_card_animates_exactly_as_a_shot_does() {
    // The claim titles were built for, cashed in. A title is media and a clip
    // cuts from it like any other, so a card that fades up needed nothing at
    // all added for titles -- and this test would be the one to notice if
    // some path had quietly special-cased them.
    use media_editor_model::Title;

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("room");
    let title = Title::line("MEDIAEDTO".into(), r(1, 6), r(1, 2), r(1, 2)).expect("a card");
    let media = project
        .add_media(MediaAsset::titled(title, RATE, frames(1_000)).expect("an asset"))
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
                item: Item::Clip(Clip::new(media, 0, frames(LENGTH)).expect("a clip")),
            },
        )
        .expect("a card on the timeline");
    project
        .apply(
            sequence,
            Edit::SetClipOpacity {
                track: 0,
                index: 0,
                opacity: Some(ramp(0, 12)),
            },
        )
        .expect("a rise");
    let layer = |tick| {
        project
            .sequence(sequence)
            .expect("a sequence")
            .stack_at(Lane::Picture, at(tick))
            .expect("a stack")[0]
            .opacity()
    };
    assert_eq!(layer(0), Rational::ZERO, "the card starts invisible");
    assert_eq!(layer(6), r(1, 2));
    assert_eq!(layer(12), Rational::ONE, "and is fully up by twelve");
}
