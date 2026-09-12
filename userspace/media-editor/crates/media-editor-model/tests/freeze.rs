// SPDX-License-Identifier: GPL-3.0-only
//! Frozen-clip behavior.
//!
//! A freeze consumes exactly one source frame and remains distinct from
//! zero-speed playback.

use media_editor_core::{Digest, Duration, Rational, Timebase};
use media_editor_model::{
    Clip, Edit, Item, MediaAsset, ModelStatus, Playback, Project, SequenceId, TrackKind,
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
fn a_freeze_shows_its_in_point_at_every_offset() {
    let still = clip().frozen();
    assert!(still.is_frozen());
    assert!(!still.is_real_time());
    assert_eq!(
        still.speed(),
        None,
        "a freeze is not a speed, however small"
    );
    for offset in [0, 1, 5, LENGTH - 1, LENGTH * 100] {
        assert_eq!(
            still.source_at(offset).expect("a tick"),
            IN_POINT,
            "at offset {offset}"
        );
    }
}

#[test]
fn a_freeze_consumes_exactly_one_frame() {
    // The claim that makes this a case of its own. A speed of nought would map
    // every offset to the in point *and* put the end there too, saying a clip
    // that shows a frame reads none of it.
    let still = clip().frozen();
    assert_eq!(still.source_end().expect("an end"), IN_POINT + 1);
    assert_eq!(
        still.source_span().expect("a span"),
        (IN_POINT, IN_POINT),
        "one frame, at both ends of the span"
    );
    // However long it runs.
    let longer = still.with_duration(frames(LENGTH * 10)).expect("a hold");
    assert_eq!(longer.source_end().expect("an end"), IN_POINT + 1);
    assert_eq!(longer.source_span().expect("a span"), (IN_POINT, IN_POINT));
}

#[test]
fn a_still_can_be_held_past_the_end_of_its_own_media() {
    // A frozen clip reads one frame, so its timeline length is not bounded by
    // the remaining source duration.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("room");
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"short"), RATE, frames(100)).expect("an asset"))
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
                item: Item::Clip(Clip::new(media, 99, frames(1)).expect("a clip")),
            },
        )
        .expect("the last frame");
    project
        .apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::Frozen,
            },
        )
        .expect("a freeze");
    assert!(
        project
            .apply(
                sequence,
                Edit::SetItemDuration {
                    track: 0,
                    index: 0,
                    duration: frames(500),
                },
            )
            .is_ok(),
        "twenty seconds of a frame there is only one of"
    );
}

#[test]
fn freezing_and_undoing_it_puts_back_the_speed_it_had() {
    // One edit rather than two, and this is the reason: its inverse has to be
    // able to say either. A clip at double speed frozen and then undone must
    // come back at double speed, not at real time.
    let (mut project, sequence) = project(TrackKind::Video);
    project
        .apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::At(r(2, 1)),
            },
        )
        .expect("double speed");
    project
        .apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::Frozen,
            },
        )
        .expect("a freeze");
    assert!(clip_at(&project, sequence, 0).is_frozen());
    project.undo(sequence).expect("an undo");
    assert_eq!(
        clip_at(&project, sequence, 0).speed(),
        Some(r(2, 1)),
        "the speed it had, not the speed it started at"
    );
    project.undo(sequence).expect("a second undo");
    assert!(clip_at(&project, sequence, 0).is_real_time());
}

#[test]
fn a_cut_through_a_freeze_makes_two_freezes_of_the_same_frame() {
    // A still cut in two is two stills of the same frame -- not a still and
    // then whatever came after it in the media, which is what a split that
    // added the offset would produce.
    let (mut project, sequence) = project(TrackKind::Video);
    project
        .apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::Frozen,
            },
        )
        .expect("a freeze");
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
    for index in 0..2 {
        let piece = clip_at(&project, sequence, index);
        assert!(piece.is_frozen(), "piece {index} is still a freeze");
        assert_eq!(piece.source_start(), IN_POINT, "of the same frame");
    }
}

#[test]
fn two_halves_of_a_freeze_join_back_into_one() {
    // Join is the exact inverse of split, which is the property that says the
    // freeze's own contiguity rule is right rather than convenient: two frozen
    // clips continue each other when they hold the *same* frame, not when the
    // second begins where the first's one frame ended.
    let (mut project, sequence) = project(TrackKind::Video);
    for edit in [
        Edit::SetClipPlayback {
            track: 0,
            index: 0,
            playback: Playback::Frozen,
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
    assert!(whole.is_frozen());
    assert_eq!(whole.source_start(), IN_POINT);
    assert_eq!(whole.duration(), frames(LENGTH));
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .items()
            .len(),
        1
    );
}

#[test]
fn two_freezes_of_different_frames_do_not_join() {
    // Which is the negative the test above needs beside it: a rule that joined
    // any two frozen clips would pass that one just as well, and would fuse
    // two stills of two different shots into one.
    let held = clip().frozen();
    let elsewhere = clip()
        .with_source(IN_POINT + 40)
        .expect("another frame")
        .frozen();
    assert!(!Item::Clip(held.clone()).continues_into(&Item::Clip(elsewhere)));
    assert!(
        Item::Clip(held.clone()).continues_into(&Item::Clip(held)),
        "and the same frame does"
    );
}

#[test]
fn a_freeze_and_a_moving_clip_do_not_join() {
    // Where the *source* question would say yes, so that only the playback
    // comparison can be what refuses. A still of frame 100 beside a clip
    // running from frame 100 satisfies the freeze's own contiguity rule
    // exactly, and joining them would keep one of the two playbacks and
    // discard the other without saying so.
    assert!(
        !Item::Clip(clip().frozen()).continues_into(&Item::Clip(clip())),
        "a still and a clip running from the same frame are two shots"
    );
    // And the other way round, because the freeze's rule is asked of the
    // *first* clip: a moving clip whose source ends where a still begins.
    let running = clip()
        .with_source(IN_POINT - LENGTH)
        .expect("the frames before")
        .with_duration(frames(LENGTH))
        .expect("a length");
    assert_eq!(
        running.source_end().expect("an end"),
        IN_POINT,
        "it really does end where the still begins"
    );
    assert!(!Item::Clip(running).continues_into(&Item::Clip(clip().frozen())));
}

#[test]
fn sound_cannot_be_frozen() {
    // A held frame of sound is a held *block* of samples, which is a tone at
    // the block rate. Silence would be a different answer, and picking one for
    // somebody is what R-1.3 forbids.
    let (mut project, sequence) = project(TrackKind::Audio);
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::Frozen,
            },
        ),
        Err(ModelStatus::SoundCannotBeRetimed)
    );
}

#[test]
fn a_gap_cannot_be_frozen() {
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
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::Frozen,
            },
        ),
        Err(ModelStatus::NotAClip)
    );
}

#[test]
fn a_freeze_keeps_everything_else_a_clip_carries() {
    // `frozen` rebuilds the clip, and a rebuild that forgot a field would
    // silently drop somebody's grade or re-time their fade.
    let (mut project, sequence) = project(TrackKind::Video);
    let grade = Digest::of(b"warm");
    for edit in [
        Edit::SetClipGrade {
            track: 0,
            index: 0,
            grade: Some(grade),
        },
        Edit::SetClipFades {
            track: 0,
            index: 0,
            fade_in: frames(4),
            fade_out: frames(6),
        },
        Edit::SetClipPlayback {
            track: 0,
            index: 0,
            playback: Playback::Frozen,
        },
    ] {
        project.apply(sequence, edit).expect("an edit");
    }
    let still = clip_at(&project, sequence, 0);
    assert!(still.is_frozen());
    assert_eq!(still.grade(), Some(grade));
    assert_eq!(still.fade_in(), frames(4));
    assert_eq!(still.fade_out(), frames(6));
    assert_eq!(still.duration(), frames(LENGTH));
}

#[test]
fn a_freeze_still_fades() {
    // The fade is about the clip's length on the timeline and the freeze is
    // about its media, so they are independent -- a still that comes up from
    // black is an ordinary thing to ask for.
    let still = clip()
        .frozen()
        .with_fades(frames(4), frames(0))
        .expect("a fade in");
    assert_eq!(still.fade_at(0).expect("a fraction"), Rational::ZERO);
    assert_eq!(still.fade_at(2).expect("a fraction"), r(1, 2));
    assert_eq!(still.fade_at(4).expect("a fraction"), Rational::ONE);
    assert_eq!(
        still.source_at(2).expect("a tick"),
        IN_POINT,
        "and it is the same frame while it comes up"
    );
}

#[test]
fn the_playhead_asks_a_freeze_for_the_frame_it_holds() {
    // Through the layer stack, which is what the renderer actually reads. The
    // stack goes through `source_at` like everything else, so this is less
    // about the freeze than about there being no second path it could miss.
    use media_editor_core::Instant;
    use media_editor_model::Lane;

    let (mut project, sequence) = project(TrackKind::Video);
    project
        .apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::Frozen,
            },
        )
        .expect("a freeze");
    for tick in [0, 1, 7, LENGTH - 1] {
        assert_eq!(
            project
                .sequence(sequence)
                .expect("a sequence")
                .stack_at(Lane::Picture, Instant::new(tick, RATE))
                .expect("a stack")[0]
                .source(),
            IN_POINT,
            "at {tick}"
        );
    }
}
