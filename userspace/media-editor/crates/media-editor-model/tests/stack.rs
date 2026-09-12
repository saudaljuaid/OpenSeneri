// SPDX-License-Identifier: GPL-3.0-only
//! What a sequence shows at an instant, and the three decisions behind it.

use media_editor_core::{Duration, Instant, Timebase};
use media_editor_model::{Clip, Edit, Item, Lane, MediaAsset, ModelStatus, Project, TrackKind};

const RATE: Timebase = Timebase::FILM_24;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a duration")
}

fn at(frame: i64) -> Instant {
    Instant::new(frame, RATE)
}

/// A project with three media assets and an empty sequence.
fn project() -> (
    Project,
    media_editor_model::SequenceId,
    [media_editor_model::MediaId; 3],
) {
    let mut project = Project::new();
    let mut media = [None; 3];
    for (index, slot) in media.iter_mut().enumerate() {
        let mut bytes = [0_u8; 32];
        bytes[0] = u8::try_from(index).expect("a byte");
        let asset = MediaAsset::new(
            media_editor_model::media::Digest::new(bytes),
            RATE,
            frames(1000),
        )
        .expect("an asset");
        *slot = Some(project.add_media(asset).expect("an identifier"));
    }
    let sequence = project.add_sequence(RATE).expect("a sequence");
    (
        project,
        sequence,
        [
            media[0].expect("one"),
            media[1].expect("two"),
            media[2].expect("three"),
        ],
    )
}

/// Put items on a track, creating it if it is not there.
///
/// Through `apply`, because every change to a project goes through an edit —
/// there is no other way in, which is what makes undo an algebra rather than a
/// snapshot (R-9.2).
fn lay(
    project: &mut Project,
    sequence: media_editor_model::SequenceId,
    track: usize,
    kind: TrackKind,
    items: &[Item],
) {
    while project
        .sequence(sequence)
        .expect("a sequence")
        .track_count()
        <= track
    {
        let index = project
            .sequence(sequence)
            .expect("a sequence")
            .track_count();
        project
            .apply(sequence, Edit::AddTrack { index, kind })
            .expect("a track");
    }
    for (index, item) in items.iter().enumerate() {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track,
                    index,
                    item: item.clone(),
                },
            )
            .expect("an insert");
    }
}

#[test]
fn higher_tracks_come_back_on_top() {
    // V1 is the bottom and V2 covers it, which is what every editing
    // application does and what every editor expects. The list comes back in
    // the order it is to be composited, so the order is data rather than a
    // convention the caller has to remember and can get backwards.
    let (mut project, sequence, media) = project();
    lay(
        &mut project,
        sequence,
        0,
        TrackKind::Video,
        &[Item::Clip(
            Clip::new(media[0], 0, frames(100)).expect("a clip"),
        )],
    );
    lay(
        &mut project,
        sequence,
        1,
        TrackKind::Video,
        &[Item::Clip(
            Clip::new(media[1], 0, frames(100)).expect("a clip"),
        )],
    );

    let stack = project
        .sequence(sequence)
        .expect("a sequence")
        .stack_at(Lane::Picture, at(50))
        .expect("a stack");
    assert_eq!(stack.len(), 2);
    assert_eq!(
        stack[0].track(),
        0,
        "V1 is first, so it is composited first"
    );
    assert_eq!(stack[0].media(), media[0]);
    assert_eq!(stack[1].track(), 1, "V2 goes over it");
    assert_eq!(stack[1].media(), media[1]);
}

#[test]
fn a_gap_is_transparent_rather_than_black() {
    // The difference between an insert edit and a hole punched in the
    // programme. A gap on V2 must let V1 through; a gap that contributed black
    // would blank out everything beneath it, and the fault would look like the
    // compositor's.
    let (mut project, sequence, media) = project();
    lay(
        &mut project,
        sequence,
        0,
        TrackKind::Video,
        &[Item::Clip(
            Clip::new(media[0], 0, frames(100)).expect("a clip"),
        )],
    );
    lay(
        &mut project,
        sequence,
        1,
        TrackKind::Video,
        &[
            Item::gap(frames(40)).expect("a gap"),
            Item::Clip(Clip::new(media[1], 0, frames(20)).expect("a clip")),
        ],
    );

    let held = project.sequence(sequence).expect("a sequence");

    // Inside V2's gap, only V1 is in the stack.
    let stack = held.stack_at(Lane::Picture, at(10)).expect("a stack");
    assert_eq!(stack.len(), 1);
    assert_eq!(stack[0].track(), 0);

    // Where V2 has material, both are.
    let stack = held.stack_at(Lane::Picture, at(50)).expect("a stack");
    assert_eq!(stack.len(), 2);

    // And past V2's material, only V1 again.
    let stack = held.stack_at(Lane::Picture, at(70)).expect("a stack");
    assert_eq!(stack.len(), 1);
    assert_eq!(stack[0].track(), 0);
}

#[test]
fn a_track_that_has_stopped_contributes_nothing() {
    // A short track is not a track full of black past its end. It has stopped,
    // which is a different statement and a different picture.
    let (mut project, sequence, media) = project();
    lay(
        &mut project,
        sequence,
        0,
        TrackKind::Video,
        &[Item::Clip(
            Clip::new(media[0], 0, frames(100)).expect("a clip"),
        )],
    );
    lay(
        &mut project,
        sequence,
        1,
        TrackKind::Video,
        &[Item::Clip(
            Clip::new(media[1], 0, frames(30)).expect("a clip"),
        )],
    );

    let held = project.sequence(sequence).expect("a sequence");
    assert_eq!(
        held.stack_at(Lane::Picture, at(29)).expect("a stack").len(),
        2
    );
    assert_eq!(
        held.stack_at(Lane::Picture, at(30)).expect("a stack").len(),
        1,
        "V2's last frame is 29, because a range is half-open"
    );

    // Past everything, the stack is empty — a real answer, not a failure.
    assert!(
        held.stack_at(Lane::Picture, at(100))
            .expect("a stack")
            .is_empty()
    );
    assert!(
        held.stack_at(Lane::Picture, at(-1))
            .expect("a stack")
            .is_empty(),
        "and before the start, likewise"
    );
}

#[test]
fn the_source_frame_is_the_clips_start_plus_how_far_in() {
    // The arithmetic that decides which frame a playhead shows. An off-by-one
    // here is wrong for the whole clip rather than for one frame of it, and it
    // is invisible on anything but a countdown or a slate.
    let (mut project, sequence, media) = project();
    lay(
        &mut project,
        sequence,
        0,
        TrackKind::Video,
        &[
            Item::gap(frames(10)).expect("a gap"),
            Item::Clip(Clip::new(media[0], 500, frames(20)).expect("a clip")),
        ],
    );
    let held = project.sequence(sequence).expect("a sequence");

    // The clip starts at timeline frame 10 and uses source frame 500 there.
    assert_eq!(
        held.stack_at(Lane::Picture, at(10)).expect("a stack")[0].source(),
        500
    );
    assert_eq!(
        held.stack_at(Lane::Picture, at(11)).expect("a stack")[0].source(),
        501
    );
    assert_eq!(
        held.stack_at(Lane::Picture, at(29)).expect("a stack")[0].source(),
        519,
        "the last frame of a twenty-frame clip is source 519, not 520"
    );
    assert!(
        held.stack_at(Lane::Picture, at(30))
            .expect("a stack")
            .is_empty()
    );
}

#[test]
fn the_lanes_do_not_see_each_other() {
    // A picture render must not be handed a sound track, and the reverse. They
    // are two stacks, asked for separately.
    let (mut project, sequence, media) = project();
    lay(
        &mut project,
        sequence,
        0,
        TrackKind::Video,
        &[Item::Clip(
            Clip::new(media[0], 0, frames(100)).expect("a clip"),
        )],
    );
    lay(
        &mut project,
        sequence,
        1,
        TrackKind::Audio,
        &[Item::Clip(
            Clip::new(media[1], 0, frames(100)).expect("a clip"),
        )],
    );
    lay(
        &mut project,
        sequence,
        2,
        TrackKind::Audio,
        &[Item::Clip(
            Clip::new(media[2], 0, frames(100)).expect("a clip"),
        )],
    );

    let held = project.sequence(sequence).expect("a sequence");
    let picture = held.stack_at(Lane::Picture, at(50)).expect("a stack");
    assert_eq!(picture.len(), 1);
    assert_eq!(picture[0].media(), media[0]);

    let sound = held.stack_at(Lane::Sound, at(50)).expect("a stack");
    assert_eq!(sound.len(), 2);
    assert_eq!(sound[0].media(), media[1]);
    assert_eq!(sound[1].media(), media[2]);
}

#[test]
fn an_instant_in_another_timebase_is_refused() {
    // A frame number means nothing without the rate it is counted at, so an
    // instant from a different sequence is not a position in this one.
    let (project, sequence, _) = project();
    let elsewhere = Instant::new(50, Timebase::PAL_25);
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .stack_at(Lane::Picture, elsewhere),
        Err(ModelStatus::Time(
            media_editor_core::CoreStatus::TimebaseMismatch
        ))
    );
}

#[test]
fn the_stack_is_the_same_stack_every_time() {
    let (mut project, sequence, media) = project();
    for (track, id) in media.iter().enumerate() {
        lay(
            &mut project,
            sequence,
            track,
            TrackKind::Video,
            &[Item::Clip(
                Clip::new(*id, i64::try_from(track).expect("a start") * 7, frames(60))
                    .expect("a clip"),
            )],
        );
    }
    let held = project.sequence(sequence).expect("a sequence");
    for frame in 0..60 {
        assert_eq!(
            held.stack_at(Lane::Picture, at(frame)).expect("a stack"),
            held.stack_at(Lane::Picture, at(frame)).expect("a stack")
        );
    }
}

#[test]
fn a_fader_is_an_edit_and_undoes_like_one() {
    // A mix level is a property of the project, so it goes in through an edit
    // and comes back out through undo — the same algebra as every other
    // change. A fader that lived outside the journal would be a change nobody
    // could take back.
    use media_editor_core::Rational;
    use media_editor_model::Fader;

    let (mut project, sequence, _) = project();
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Audio,
            },
        )
        .expect("a track");
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .fader(),
        Fader::UNITY,
        "a new track is at unity, not at silence"
    );

    let quiet = Fader::at(Rational::new(-12, 1).expect("a ratio")).expect("a level");
    project
        .apply(
            sequence,
            Edit::SetTrackFader {
                track: 0,
                fader: quiet,
            },
        )
        .expect("a fader move");
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .fader(),
        quiet
    );

    project.undo(sequence).expect("an undo");
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .fader(),
        Fader::UNITY,
        "undo returns the fader to where it was, not to a default"
    );

    project.redo(sequence).expect("a redo");
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .fader(),
        quiet
    );

    // Moving it twice and undoing twice walks back through both positions,
    // which is what makes it an algebra rather than a pair of snapshots.
    let quieter = Fader::at(Rational::new(-24, 1).expect("a ratio")).expect("a level");
    project
        .apply(
            sequence,
            Edit::SetTrackFader {
                track: 0,
                fader: quieter,
            },
        )
        .expect("a fader move");
    project.undo(sequence).expect("an undo");
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .fader(),
        quiet,
        "back to the first position, not to unity"
    );
}

#[test]
fn muted_is_not_the_same_as_turned_all_the_way_down() {
    // The logarithm of zero is not a point on the decibel scale, so "off" is a
    // separate detent. A format or an interface that conflated them could not
    // restore a level on unmute.
    use media_editor_core::Rational;
    use media_editor_model::{Fader, MINIMUM_DECIBELS};

    let quietest =
        Fader::at(Rational::new(MINIMUM_DECIBELS, 1).expect("a ratio")).expect("a level");
    assert!(!quietest.is_muted());
    assert!(Fader::MUTED.is_muted());
    assert_eq!(Fader::MUTED.decibels(), None);
    assert_ne!(quietest, Fader::MUTED);
}

#[test]
fn the_fader_has_ends() {
    use media_editor_core::Rational;
    use media_editor_model::{Fader, MAXIMUM_DECIBELS, MINIMUM_DECIBELS};

    assert!(Fader::at(Rational::new(MINIMUM_DECIBELS, 1).expect("a ratio")).is_ok());
    assert!(Fader::at(Rational::new(MAXIMUM_DECIBELS, 1).expect("a ratio")).is_ok());
    assert_eq!(
        Fader::at(Rational::new(MINIMUM_DECIBELS - 1, 1).expect("a ratio")),
        Err(ModelStatus::FaderOutOfRange)
    );
    assert_eq!(
        Fader::at(Rational::new(MAXIMUM_DECIBELS + 1, 1).expect("a ratio")),
        Err(ModelStatus::FaderOutOfRange)
    );
}

/// A track of two clips, each `each` frames long, with `handle` frames of
/// source before the second clip's in point.
fn two_clips(
    project: &mut Project,
    sequence: media_editor_model::SequenceId,
    media: [media_editor_model::MediaId; 3],
    each: i64,
    handle: i64,
) {
    lay(
        project,
        sequence,
        0,
        TrackKind::Video,
        &[
            Item::Clip(Clip::new(media[0], 0, frames(each)).expect("a clip")),
            Item::Clip(Clip::new(media[1], handle, frames(each)).expect("a clip")),
        ],
    );
}

#[test]
fn a_dissolve_puts_both_sides_of_the_cut_on_screen() {
    // The defining property. Outside a dissolve one clip is showing; inside
    // it, two are, and they are the two the cut is between.
    use media_editor_model::Transition;

    let (mut project, sequence, media) = project();
    two_clips(&mut project, sequence, media, 20, 100);
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(4)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");
    let held = project.sequence(sequence).expect("a sequence");

    // Four frames, centred on the cut at frame 20: frames 18, 19, 20, 21.
    for frame in [0, 17, 22, 39] {
        assert_eq!(
            held.stack_at(Lane::Picture, at(frame))
                .expect("a stack")
                .len(),
            1,
            "outside the dissolve at {frame}"
        );
    }
    for frame in 18..22 {
        let stack = held.stack_at(Lane::Picture, at(frame)).expect("a stack");
        assert_eq!(stack.len(), 2, "inside the dissolve at {frame}");
        assert_eq!(
            stack[0].media(),
            media[0],
            "the outgoing clip is underneath"
        );
        assert_eq!(stack[1].media(), media[1], "the incoming one fades up");
    }
}

#[test]
fn a_dissolve_never_shows_only_one_of_its_two_clips() {
    // An N-frame dissolve runs from 1/(N+1) to N/(N+1) and touches neither end.
    // A dissolve whose first frame is entirely the outgoing clip has spent a
    // frame showing what the frame before it already showed.
    use media_editor_core::Rational;
    use media_editor_model::Transition;

    let (mut project, sequence, media) = project();
    two_clips(&mut project, sequence, media, 20, 100);
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(4)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");
    let held = project.sequence(sequence).expect("a sequence");

    let expected = [
        Rational::new(1, 5).expect("a ratio"),
        Rational::new(2, 5).expect("a ratio"),
        Rational::new(3, 5).expect("a ratio"),
        Rational::new(4, 5).expect("a ratio"),
    ];
    for (offset, want) in expected.iter().enumerate() {
        let frame = 18 + i64::try_from(offset).expect("a frame");
        let stack = held.stack_at(Lane::Picture, at(frame)).expect("a stack");
        assert_eq!(
            stack[0].opacity(),
            Rational::ONE,
            "the outgoing clip is whole"
        );
        assert_eq!(stack[1].opacity(), *want, "at frame {frame}");
        assert_ne!(stack[1].opacity(), Rational::ZERO);
        assert_ne!(stack[1].opacity(), Rational::ONE);
    }
}

#[test]
fn a_dissolve_reaches_into_the_handles_on_both_sides() {
    // The outgoing clip runs past its own out point and the incoming one
    // starts before its in point. That is what handles are, and the source
    // positions have to say so — an implementation that clamped instead would
    // freeze a frame at each end of every dissolve.
    use media_editor_model::Transition;

    let (mut project, sequence, media) = project();
    two_clips(&mut project, sequence, media, 20, 100);
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(4)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");
    let held = project.sequence(sequence).expect("a sequence");

    // The outgoing clip is media[0] starting at source 0, twenty frames long,
    // so its last ordinary frame is source 19. Frames 20 and 21 of the
    // timeline are sources 20 and 21 — past its out point, into handles.
    let sources: std::vec::Vec<i64> = (18..22)
        .map(|frame| held.stack_at(Lane::Picture, at(frame)).expect("a stack")[0].source())
        .collect();
    assert_eq!(sources, std::vec![18, 19, 20, 21]);

    // The incoming clip's in point is source 100 at timeline frame 20, so at
    // frames 18 and 19 it is at 98 and 99 — before its in point.
    let sources: std::vec::Vec<i64> = (18..22)
        .map(|frame| held.stack_at(Lane::Picture, at(frame)).expect("a stack")[1].source())
        .collect();
    assert_eq!(sources, std::vec![98, 99, 100, 101]);
}

#[test]
fn an_odd_dissolve_puts_its_extra_frame_after_the_cut() {
    // A five-frame dissolve cannot be centred on a cut, so the choice is
    // written down rather than left to whichever way integer division
    // happened to go: two frames before, three after.
    use media_editor_model::Transition;

    let (mut project, sequence, media) = project();
    two_clips(&mut project, sequence, media, 20, 100);
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(5)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");
    let held = project.sequence(sequence).expect("a sequence");

    assert_eq!(
        held.stack_at(Lane::Picture, at(17)).expect("a stack").len(),
        1
    );
    for frame in 18..23 {
        assert_eq!(
            held.stack_at(Lane::Picture, at(frame))
                .expect("a stack")
                .len(),
            2,
            "at {frame}"
        );
    }
    assert_eq!(
        held.stack_at(Lane::Picture, at(23)).expect("a stack").len(),
        1
    );
}

#[test]
fn a_dissolve_is_an_edit_and_undoes_like_one() {
    use media_editor_model::Transition;

    let (mut project, sequence, media) = project();
    two_clips(&mut project, sequence, media, 20, 100);
    let transition = Transition::new(1, frames(4)).expect("a dissolve");
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition,
            },
        )
        .expect("a dissolve");
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .transition_at(1),
        Some(transition)
    );

    project.undo(sequence).expect("an undo");
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .transition_at(1),
        None
    );

    project.redo(sequence).expect("a redo");
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .transition_at(1),
        Some(transition),
        "and the redo restores the same dissolve, not a default one"
    );
}

#[test]
fn a_dissolve_needs_a_cut_between_two_clips() {
    use media_editor_model::Transition;

    let (mut project, sequence, media) = project();

    // The start of a track is not a cut between two things.
    assert_eq!(Transition::new(0, frames(4)), Err(ModelStatus::UnknownItem));
    assert_eq!(
        Transition::new(1, media_editor_core::Duration::zero(RATE)),
        Err(ModelStatus::EmptyItem)
    );

    // A dissolve to a gap is a dissolve to nothing, which is not a dissolve.
    lay(
        &mut project,
        sequence,
        0,
        TrackKind::Video,
        &[
            Item::Clip(Clip::new(media[0], 0, frames(20)).expect("a clip")),
            Item::gap(frames(20)).expect("a gap"),
        ],
    );
    assert_eq!(
        project.apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(4)).expect("a dissolve"),
            },
        ),
        Err(ModelStatus::NotAClip)
    );

    // And a cut past the end of the track names nothing.
    assert_eq!(
        project.apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(9, frames(4)).expect("a dissolve"),
            },
        ),
        Err(ModelStatus::UnknownItem)
    );
}

#[test]
fn a_dissolve_may_not_outlast_its_clips_or_run_off_the_front_of_one() {
    use media_editor_model::Transition;

    let (mut generous, sequence, media) = project();
    two_clips(&mut generous, sequence, media, 20, 100);
    assert_eq!(
        generous.apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(21)).expect("a dissolve"),
            },
        ),
        Err(ModelStatus::TransitionTooLong),
        "a dissolve longer than the clip it dissolves from needs material from \
         before that clip began"
    );

    // The incoming clip needs handles before its in point. This one starts at
    // source 1, so it has one frame of handle and a four-frame dissolve wants
    // two.
    let (mut tight, sequence, media) = project();
    two_clips(&mut tight, sequence, media, 20, 1);
    assert_eq!(
        tight.apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(4)).expect("a dissolve"),
            },
        ),
        Err(ModelStatus::SourceBeforeStart)
    );

    // Two frames of handle is exactly enough for a four-frame dissolve.
    let (mut exact, sequence, media) = project();
    two_clips(&mut exact, sequence, media, 20, 2);
    assert!(
        exact
            .apply(
                sequence,
                Edit::AddTransition {
                    track: 0,
                    transition: Transition::new(1, frames(4)).expect("a dissolve"),
                },
            )
            .is_ok()
    );
}

#[test]
fn an_edit_that_would_move_a_dissolve_is_refused() {
    // Inserting or removing an item renumbers every cut after it, which would
    // slide a dissolve onto a cut nobody put it on. Rather than renumber them
    // — and have to renumber them back exactly on undo — the edit is refused
    // while the dissolve is in the way.
    use media_editor_model::Transition;

    let (mut project, sequence, media) = project();
    two_clips(&mut project, sequence, media, 20, 100);
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(4)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");

    assert_eq!(
        project.apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::gap(frames(5)).expect("a gap"),
            },
        ),
        Err(ModelStatus::TransitionInTheWay)
    );
    assert_eq!(
        project.apply(sequence, Edit::RemoveItem { track: 0, index: 0 }),
        Err(ModelStatus::TransitionInTheWay)
    );

    // Appending past the dissolve is fine: it renumbers nothing.
    assert!(
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index: 2,
                    item: Item::gap(frames(5)).expect("a gap"),
                },
            )
            .is_ok()
    );

    // And once the dissolve is off, the refused edit goes through.
    project
        .apply(
            sequence,
            Edit::RemoveTransition {
                track: 0,
                boundary: 1,
            },
        )
        .expect("a removal");
    assert!(
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index: 0,
                    item: Item::gap(frames(5)).expect("a gap"),
                },
            )
            .is_ok()
    );
}

#[test]
fn a_cut_may_hold_only_one_dissolve() {
    use media_editor_model::Transition;

    let (mut project, sequence, media) = project();
    two_clips(&mut project, sequence, media, 20, 100);
    let transition = Transition::new(1, frames(4)).expect("a dissolve");
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition,
            },
        )
        .expect("a dissolve");
    assert_eq!(
        project.apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition
            }
        ),
        Err(ModelStatus::TransitionExists)
    );
    assert_eq!(
        project.apply(
            sequence,
            Edit::RemoveTransition {
                track: 0,
                boundary: 2,
            },
        ),
        Err(ModelStatus::UnknownTransition)
    );
}

#[test]
fn a_wipe_is_timed_exactly_like_a_dissolve() {
    // The whole design claim: a wipe and a dissolve of the same length at the
    // same cut are the same transition, and differ only in what the renderer
    // does with the fraction. So both sides are on screen over exactly the
    // same frames, and the fraction runs through exactly the same values.
    use media_editor_model::{Transition, Wipe};

    let (mut fading, sequence, media) = project();
    two_clips(&mut fading, sequence, media, 20, 100);
    let mut wiping = fading.clone();
    fading
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(4)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");
    wiping
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::wiping(1, frames(4), Wipe::RIGHTWARD).expect("a wipe"),
            },
        )
        .expect("a wipe");

    let dissolved = fading.sequence(sequence).expect("a sequence");
    let wiped = wiping.sequence(sequence).expect("a sequence");
    for frame in 0..40 {
        let one = dissolved
            .stack_at(Lane::Picture, at(frame))
            .expect("a stack");
        let other = wiped.stack_at(Lane::Picture, at(frame)).expect("a stack");
        assert_eq!(one.len(), other.len(), "at {frame}");
        for (a, b) in one.iter().zip(&other) {
            assert_eq!(a.media(), b.media(), "at {frame}");
            assert_eq!(a.source(), b.source(), "at {frame}");
        }
    }
    // And the fraction is the same number, spent differently: the dissolve
    // spends it on the incoming layer's opacity, the wipe carries it.
    for frame in 18..22 {
        let dissolving = dissolved
            .stack_at(Lane::Picture, at(frame))
            .expect("a stack");
        let sweeping = wiped.stack_at(Lane::Picture, at(frame)).expect("a stack");
        assert_eq!(
            sweeping[1].wipe().expect("a wipe").fraction(),
            dissolving[1].opacity(),
            "at {frame}"
        );
    }
}

#[test]
fn a_wipe_leaves_both_of_its_clips_whole() {
    // The incoming clip of a wipe is not half-faded: it is entirely there,
    // behind an edge. Anything that spent the fraction on its opacity as well
    // would show it through the outgoing one on the covered side.
    use media_editor_core::Rational;
    use media_editor_model::{Transition, Wipe};

    let (mut project, sequence, media) = project();
    two_clips(&mut project, sequence, media, 20, 100);
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::wiping(1, frames(4), Wipe::DOWNWARD).expect("a wipe"),
            },
        )
        .expect("a wipe");
    let held = project.sequence(sequence).expect("a sequence");
    for frame in 18..22 {
        let stack = held.stack_at(Lane::Picture, at(frame)).expect("a stack");
        assert_eq!(stack[0].opacity(), Rational::ONE, "outgoing at {frame}");
        assert_eq!(stack[1].opacity(), Rational::ONE, "incoming at {frame}");
        assert!(stack[0].wipe().is_none(), "the outgoing side is whole");
        assert_eq!(stack[1].wipe().expect("a wipe").wipe(), Wipe::DOWNWARD);
    }
}

#[test]
fn a_wipe_inside_a_fade_is_still_faded() {
    // A track's automation multiplies whatever the items on it are doing, and
    // a wipe is no exception: the transition decides which side of the edge a
    // pixel is on, the track decides how much of the whole track is showing,
    // and either alone throws the other away.
    use media_editor_core::{Instant, Rational};
    use media_editor_model::curve::{Curve, Interpolation, Keyframe};
    use media_editor_model::{Transition, Wipe};

    let (mut project, sequence, media) = project();
    two_clips(&mut project, sequence, media, 20, 100);
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::wiping(1, frames(4), Wipe::RIGHTWARD).expect("a wipe"),
            },
        )
        .expect("a wipe");
    let opacity = Curve::new(vec![
        Keyframe::new(Instant::new(0, RATE), Rational::ONE, Interpolation::Hold)
            .expect("a keyframe"),
        Keyframe::new(
            Instant::new(19, RATE),
            Rational::new(1, 2).expect("a half"),
            Interpolation::Hold,
        )
        .expect("a keyframe"),
    ])
    .expect("a curve");
    project
        .apply(
            sequence,
            Edit::SetTrackOpacity {
                track: 0,
                opacity: Some(opacity),
            },
        )
        .expect("a curve");
    let held = project.sequence(sequence).expect("a sequence");
    let stack = held.stack_at(Lane::Picture, at(20)).expect("a stack");
    assert_eq!(stack[1].opacity(), Rational::new(1, 2).expect("a half"));
    assert!(
        stack[1].wipe().is_some(),
        "the wipe is still there underneath the fade"
    );
}

#[test]
fn a_wipe_with_no_direction_is_refused() {
    use media_editor_core::Rational;
    use media_editor_model::{ModelStatus, Wipe};
    assert_eq!(
        Wipe::new(Rational::ZERO, Rational::ZERO),
        Err(ModelStatus::DegenerateWipe)
    );
}

#[test]
fn a_softness_past_its_own_range_is_refused() {
    use media_editor_core::Rational;
    use media_editor_model::{ModelStatus, Wipe};
    assert_eq!(
        Wipe::soft(
            Rational::ONE,
            Rational::ZERO,
            Rational::new(3, 2).expect("a rational")
        ),
        Err(ModelStatus::SoftnessOutOfRange),
        "past one the ramp would start before the wipe does"
    );
    assert_eq!(
        Wipe::soft(
            Rational::ONE,
            Rational::ZERO,
            Rational::new(-1, 4).expect("a rational")
        ),
        Err(ModelStatus::SoftnessOutOfRange)
    );
    // And nought is a hard edge rather than a refusal, because a slider at its
    // low end is a thing somebody set rather than a mistake.
    assert!(Wipe::soft(Rational::ONE, Rational::ZERO, Rational::ZERO).is_ok());
}
