// SPDX-License-Identifier: GPL-3.0-only
//! A track whose opacity changes over time.
//!
//! The curve type has its own tests; these are about what happens when one is
//! attached to a track — how it reaches the layer stack, how it undoes, and
//! what it refuses.

use media_editor_core::{Duration, Instant, Rational, Timebase};
use media_editor_model::curve::{Curve, Interpolation, Keyframe};
use media_editor_model::{Clip, Edit, Item, Lane, MediaAsset, ModelStatus, Project, TrackKind};

const RATE: Timebase = Timebase::FILM_24;

fn at(frame: i64) -> Instant {
    Instant::new(frame, RATE)
}

fn value(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a value")
}

/// A curve from nought to one over `span` frames, linear.
fn fade_up(span: i64) -> Curve {
    Curve::new(std::vec![
        Keyframe::new(at(0), value(0, 1), Interpolation::Linear).expect("a keyframe"),
        Keyframe::new(at(span), value(1, 1), Interpolation::Hold).expect("a keyframe"),
    ])
    .expect("a curve")
}

/// A project with one picture track holding one clip, and one sound track.
fn project() -> (Project, media_editor_model::SequenceId) {
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
            Edit::AddTrack {
                index: 1,
                kind: TrackKind::Audio,
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
                    Clip::new(media, 0, Duration::new(48, RATE).expect("a duration"))
                        .expect("a clip"),
                ),
            },
        )
        .expect("a clip");
    (project, sequence)
}

#[test]
fn a_track_with_no_automation_is_fully_opaque() {
    // The default, and the thing every existing test depended on without
    // saying so. Nothing means opaque, and it stays that way.
    let (project, sequence) = project();
    let held = project.sequence(sequence).expect("a sequence");
    assert!(held.track(0).expect("a track").opacity().is_none());
    for frame in [0, 24, 47] {
        assert_eq!(
            held.track(0).expect("a track").opacity_at(at(frame)),
            Ok(Rational::ONE)
        );
        assert_eq!(
            held.stack_at(Lane::Picture, at(frame)).expect("a stack")[0].opacity(),
            Rational::ONE
        );
    }
}

#[test]
fn an_animated_track_reaches_the_layer_stack() {
    // The whole point: a curve on a track has to arrive where the compositor
    // will see it. A fade over 24 frames must read n/24 at frame n in the
    // stack, exactly — the same numbers the curve gives, unchanged in transit.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetTrackOpacity {
                track: 0,
                opacity: Some(fade_up(24)),
            },
        )
        .expect("an automation");
    let held = project.sequence(sequence).expect("a sequence");
    for frame in 0..=24 {
        let stack = held.stack_at(Lane::Picture, at(frame)).expect("a stack");
        assert_eq!(
            stack[0].opacity(),
            value(frame, 24),
            "frame {frame} did not reach the stack"
        );
    }
    // And past the last keyframe it holds, because the curve does.
    assert_eq!(
        held.stack_at(Lane::Picture, at(40)).expect("a stack")[0].opacity(),
        Rational::ONE
    );
}

#[test]
fn an_overshoot_saturates_rather_than_becoming_more_than_opaque() {
    // A curve may overshoot on purpose, and nothing can be more than fully
    // opaque or less than fully clear. Saturating at the limit is what the
    // limit means — which is a different decision from refusing an ease handle
    // outside its span: that one changes which curve was drawn, this one is
    // the curve the editor drew meeting a physical fact.
    let over = Curve::new(std::vec![
        Keyframe::new(at(0), value(-1, 2), Interpolation::Linear).expect("a keyframe"),
        Keyframe::new(at(24), value(3, 2), Interpolation::Hold).expect("a keyframe"),
    ])
    .expect("a curve");
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetTrackOpacity {
                track: 0,
                opacity: Some(over),
            },
        )
        .expect("an automation");
    let held = project.sequence(sequence).expect("a sequence");

    // Below nought reads as nought, above one reads as one, and the part in
    // between is untouched.
    assert_eq!(
        held.stack_at(Lane::Picture, at(0)).expect("a stack")[0].opacity(),
        Rational::ZERO
    );
    assert_eq!(
        held.stack_at(Lane::Picture, at(24)).expect("a stack")[0].opacity(),
        Rational::ONE
    );
    // At frame 12 the curve is at a half exactly: -1/2 plus half of 2.
    assert_eq!(
        held.stack_at(Lane::Picture, at(12)).expect("a stack")[0].opacity(),
        value(1, 2)
    );
}

#[test]
fn automation_multiplies_a_dissolve_rather_than_replacing_it() {
    // Two things are deciding a layer's opacity at a dissolve inside an
    // animated fade, and they compose: the dissolve says how much of the
    // incoming clip is showing, the automation says how much of the track is.
    // Either one alone would throw away the other.
    let (mut project, sequence) = project();
    let media = project.media().iter().next().expect("media").0;
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 1,
                // Twenty-four frames in, so the incoming side has handles: a
                // dissolve needs media before the cut on the clip that is
                // arriving, and a clip that starts at the head of its media
                // has none.
                item: Item::Clip(
                    Clip::new(media, 24, Duration::new(48, RATE).expect("a duration"))
                        .expect("a clip"),
                ),
            },
        )
        .expect("a clip");
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: media_editor_model::Transition::new(
                    1,
                    Duration::new(8, RATE).expect("a duration"),
                )
                .expect("a dissolve"),
            },
        )
        .expect("a dissolve");

    // Half opacity across the whole track.
    let half = Curve::constant(at(0), value(1, 2)).expect("a curve");
    project
        .apply(
            sequence,
            Edit::SetTrackOpacity {
                track: 0,
                opacity: Some(half),
            },
        )
        .expect("an automation");

    let held = project.sequence(sequence).expect("a sequence");
    let stack = held.stack_at(Lane::Picture, at(46)).expect("a stack");
    assert_eq!(stack.len(), 2, "the dissolve did not put two layers up");
    // The outgoing side is at full within the dissolve, halved by the track.
    assert_eq!(stack[0].opacity(), value(1, 2));
    // The incoming side is at a dissolve fraction, halved by the track — so it
    // is strictly between nothing and a half, and not a half.
    assert!(stack[1].opacity() > Rational::ZERO);
    assert!(stack[1].opacity() < value(1, 2));
}

#[test]
fn setting_an_automation_undoes_and_redoes() {
    // Automation is a property of the project, so it is an edit like every
    // other: it has an inverse, it undoes, and the journal checks that undoing
    // it reproduces what was applied.
    let (mut project, sequence) = project();
    let curve = fade_up(24);
    project
        .apply(
            sequence,
            Edit::SetTrackOpacity {
                track: 0,
                opacity: Some(curve.clone()),
            },
        )
        .expect("an automation");
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .opacity(),
        Some(&curve)
    );

    project.undo(sequence).expect("an undo");
    assert!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .opacity()
            .is_none(),
        "undo did not take the automation off"
    );

    project.redo(sequence).expect("a redo");
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .opacity(),
        Some(&curve),
        "redo did not put the same curve back"
    );
}

#[test]
fn switching_automation_off_and_on_keeps_the_shape() {
    // Why `None` is not the same as a curve holding one. Taking automation off
    // and putting it back must return the shape somebody drew, not a flat
    // line — which is what a design that stored "opacity is 1 everywhere"
    // instead of "there is no automation" would give back.
    let (mut project, sequence) = project();
    let curve = fade_up(24);
    project
        .apply(
            sequence,
            Edit::SetTrackOpacity {
                track: 0,
                opacity: Some(curve.clone()),
            },
        )
        .expect("an automation");
    project
        .apply(
            sequence,
            Edit::SetTrackOpacity {
                track: 0,
                opacity: None,
            },
        )
        .expect("an automation");
    assert!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .opacity()
            .is_none()
    );

    // Undo puts the shape back rather than a flat line, which is what a design
    // that stored "opacity is one everywhere" instead of "there is no
    // automation" would give.
    project.undo(sequence).expect("an undo");
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .opacity(),
        Some(&curve),
        "the shape did not come back"
    );
}

#[test]
fn an_automated_fader_saturates_at_the_ends_of_its_travel() {
    // A curve may overshoot on purpose, and a fader's travel is a physical end
    // stop. An ease from nought to twenty-four decibels with its incoming
    // handle at two reaches `6(1-t)t² + t³`, which at three quarters is
    // `54/64 + 27/64 = 81/64` — so the curve asks for `24 × 81/64 = 30.375`
    // decibels, past the end of the fader.
    //
    // The horizontal handles are a third and two thirds, where `x(t) = t`, so
    // frame 18 of 24 is exactly `t = 3/4` and the overshoot lands on a frame
    // rather than between two.
    //
    // This test exists because the clamp failed no control without it: an
    // unclamped value does not produce a wrong number, it produces a
    // `FaderOutOfRange` refusal — and no fixture had a curve that could reach
    // one. A guard whose only witness is a refusal nothing triggers is a guard
    // nothing checks.
    let over = Curve::new(std::vec![
        Keyframe::new(
            at(0),
            value(0, 1),
            Interpolation::Ease {
                out_x: value(1, 3),
                out_y: value(0, 1),
                in_x: value(2, 3),
                in_y: value(2, 1),
            },
        )
        .expect("a keyframe"),
        Keyframe::new(at(24), value(24, 1), Interpolation::Hold).expect("a keyframe"),
    ])
    .expect("a curve");

    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetTrackLevel {
                track: 1,
                level: Some(over),
            },
        )
        .expect("an automation");
    let track = project
        .sequence(sequence)
        .expect("a sequence")
        .track(1)
        .expect("a track");

    assert_eq!(
        track.fader_at(at(18)).expect("a fader").decibels(),
        Some(value(24, 1)),
        "the overshoot was not held at the top of the travel"
    );
    // Below the top it is the curve, untouched.
    assert_eq!(
        track.fader_at(at(0)).expect("a fader").decibels(),
        Some(value(0, 1))
    );

    // And the floor, from the other direction.
    let under = Curve::new(std::vec![
        Keyframe::new(at(0), value(-200, 1), Interpolation::Hold).expect("a keyframe"),
        Keyframe::new(at(24), value(0, 1), Interpolation::Hold).expect("a keyframe"),
    ])
    .expect("a curve");
    project
        .apply(
            sequence,
            Edit::SetTrackLevel {
                track: 1,
                level: Some(under),
            },
        )
        .expect("an automation");
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(1)
            .expect("a track")
            .fader_at(at(0))
            .expect("a fader")
            .decibels(),
        Some(value(media_editor_model::MINIMUM_DECIBELS, 1)),
        "a value below the floor was not held at it"
    );
}

#[test]
fn an_opacity_on_a_sound_track_is_refused() {
    // A sound track's level is its fader, in decibels. An opacity on one would
    // be a second level with different units that nothing reads, which is
    // worse than no level at all.
    let (mut project, sequence) = project();
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetTrackOpacity {
                track: 1,
                opacity: Some(fade_up(24)),
            },
        ),
        Err(ModelStatus::OpacityOnSound)
    );
}

#[test]
fn an_automation_counted_another_way_is_refused() {
    // Frame 12 at 24 and frame 12 at 25 are different moments, and a curve
    // counted one way on a track counted another would read every keyframe at
    // the wrong instant.
    let other = Curve::new(std::vec![
        Keyframe::new(
            Instant::new(0, Timebase::PAL_25),
            value(0, 1),
            Interpolation::Linear,
        )
        .expect("a keyframe"),
        Keyframe::new(
            Instant::new(25, Timebase::PAL_25),
            value(1, 1),
            Interpolation::Hold,
        )
        .expect("a keyframe"),
    ])
    .expect("a curve");
    let (mut project, sequence) = project();
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetTrackOpacity {
                track: 0,
                opacity: Some(other),
            },
        ),
        Err(ModelStatus::WrongTimebase)
    );
}
