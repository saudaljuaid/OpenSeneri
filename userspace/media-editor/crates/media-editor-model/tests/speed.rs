// SPDX-License-Identifier: GPL-3.0-only
//! Retiming: a clip that plays its media at a speed somebody chose.
//!
//! The clip keeps its length on the timeline. What changes is how much media
//! it consumes to fill that length — so a half is slow motion, a two is fast,
//! and a negative runs the media backwards from the in point.
//!
//! The speed is an **exact rational** and that is not decoration. A clip at
//! 24/25 is the standard PAL pull-down; a clip at 0.96 is a rounding of it
//! that drifts a frame every twenty-five seconds. A speed nobody can write
//! down exactly is a speed two builds can disagree about.

use media_editor_core::{Digest, Duration, Rational, Timebase};
use media_editor_model::{
    Clip, Edit, Item, Lane, MediaAsset, ModelStatus, Playback, Project, SequenceId, TrackKind,
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

/// A project with one clip on a picture track, in at [`IN_POINT`].
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
    clip_at(&project(TrackKind::Video).0, project(TrackKind::Video).1, 0)
}

/// The media ticks a clip shows over its first `count` frames.
fn ticks(held: &Clip, count: i64) -> Vec<i64> {
    (0..count)
        .map(|offset| held.source_at(offset).expect("a tick"))
        .collect()
}

#[test]
fn real_time_is_the_in_point_plus_the_offset() {
    // Which the general mapping gives without an arm of its own: the size of
    // a speed of one is one, and `floor(offset x 1)` is the offset.
    let held = clip();
    assert!(held.is_real_time());
    assert_eq!(held.speed(), Some(Rational::ONE));
    assert_eq!(
        ticks(&held, 5),
        vec![
            IN_POINT,
            IN_POINT + 1,
            IN_POINT + 2,
            IN_POINT + 3,
            IN_POINT + 4
        ]
    );
}

#[test]
fn a_half_shows_each_frame_twice_and_a_two_shows_every_other() {
    let slow = clip().with_speed(r(1, 2)).expect("half speed");
    assert_eq!(
        ticks(&slow, 6),
        vec![100, 100, 101, 101, 102, 102],
        "two ticks of the timeline for one of the media"
    );
    let fast = clip().with_speed(r(2, 1)).expect("double speed");
    assert_eq!(ticks(&fast, 6), vec![100, 102, 104, 106, 108, 110]);
    assert!(!slow.is_real_time() && !fast.is_real_time());
}

#[test]
fn a_speed_no_decimal_can_write_is_exact() {
    // Twenty-four twenty-fifths: the standard pull-down, and the case a
    // floating speed gets wrong slowly enough that nobody sees it until a
    // delivery. At twenty-five frames in the media has advanced exactly
    // twenty-four, and at twenty-six it has advanced twenty-four still --
    // because 624/25 is 24.96 and a tick names a frame rather than a moment.
    let held = clip().with_speed(r(24, 25)).expect("a pull-down");
    assert_eq!(held.source_at(0).expect("a tick"), 100);
    assert_eq!(held.source_at(25).expect("a tick"), 124);
    assert_eq!(held.source_at(26).expect("a tick"), 124);
    assert_eq!(held.source_at(50).expect("a tick"), 148);
}

#[test]
fn a_negative_speed_runs_the_media_backwards_from_the_in_point() {
    let held = clip().with_speed(r(-1, 1)).expect("reverse");
    assert_eq!(ticks(&held, 4), vec![100, 99, 98, 97]);
    // And a reversed clip shows exactly the frames its forward twin shows,
    // because the *size* of the speed decides how much media passes and the
    // sign only decides which way. Flooring `offset x speed` directly would
    // round the other way for a negative and give `100, 99, 99, 98` -- the in
    // point once and everything after it twice.
    let slow = clip().with_speed(r(-1, 2)).expect("reverse, slowly");
    let forward = clip().with_speed(r(1, 2)).expect("forwards, slowly");
    assert_eq!(ticks(&slow, 6), vec![100, 100, 99, 99, 98, 98]);
    for offset in 0..6 {
        assert_eq!(
            IN_POINT - slow.source_at(offset).expect("a tick"),
            forward.source_at(offset).expect("a tick") - IN_POINT,
            "at {offset}, the two are the same distance from the in point"
        );
    }
}

#[test]
fn a_reverse_that_would_read_before_its_media_is_refused() {
    // Checked when the speed is set rather than at the frame that reads it:
    // the editor who set it is the one who can do something about it.
    let held = clip();
    assert!(
        held.with_speed(r(-4, 1)).is_ok(),
        "twenty-three frames back at four times is ninety-two, which fits"
    );
    assert_eq!(
        held.with_speed(r(-5, 1)),
        Err(ModelStatus::SourceBeforeStart),
        "twenty-three at five times is a hundred and fifteen, which does not"
    );
}

#[test]
fn a_speed_of_nothing_is_refused() {
    // It would show one frame forever and consume no media, which is a freeze
    // -- a different edit with a different name, and one nobody has asked for.
    assert_eq!(
        clip().with_speed(Rational::ZERO),
        Err(ModelStatus::SpeedNotUsable)
    );
}

#[test]
fn the_source_end_is_past_what_the_clip_consumes() {
    // Not past its length on the timeline. That is what makes a join between
    // two retimed clips ask the right question.
    let held = clip().with_speed(r(2, 1)).expect("double speed");
    assert_eq!(
        held.source_end().expect("an end"),
        IN_POINT + 2 * LENGTH,
        "a clip at double speed eats twice its length of media"
    );
    assert_eq!(
        clip().source_end().expect("an end"),
        IN_POINT + LENGTH,
        "and one at real time eats its length"
    );
}

#[test]
fn a_cut_through_a_retimed_clip_does_not_re_time_it() {
    // The tail begins where the head left off *in the media*, which at a
    // speed other than one is `offset x speed` in and not `offset` in. Adding
    // the offset would look like a frame of drift rather than like a bug.
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
    let whole = ticks(&clip_at(&project, sequence, 0), LENGTH);
    project
        .apply(
            sequence,
            Edit::SplitItem {
                track: 0,
                index: 0,
                offset: 10,
            },
        )
        .expect("a cut");
    let head = ticks(&clip_at(&project, sequence, 0), 10);
    let tail = ticks(&clip_at(&project, sequence, 1), LENGTH - 10);
    let joined: Vec<i64> = head.into_iter().chain(tail).collect();
    assert_eq!(
        joined, whole,
        "a cut shows exactly what was there before it"
    );
}

#[test]
fn a_cut_through_a_retimed_clip_is_undone_by_a_join() {
    let (mut project, sequence) = project(TrackKind::Video);
    project
        .apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::At(r(3, 2)),
            },
        )
        .expect("a speed");
    let original = project.sequence(sequence).expect("a sequence").clone();
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
    project
        .apply(sequence, Edit::JoinItems { track: 0, index: 0 })
        .expect("a join");
    assert_eq!(project.sequence(sequence).expect("a sequence"), &original);
}

#[test]
fn two_clips_at_different_speeds_do_not_join() {
    // Adjacent in their source and identical in every other way, so only the
    // speed can refuse them -- and it must, because joining would play the
    // second's half at the first's rate without saying so.
    let (mut project, sequence) = project(TrackKind::Video);
    project
        .apply(
            sequence,
            Edit::SplitItem {
                track: 0,
                index: 0,
                offset: 12,
            },
        )
        .expect("a cut");
    project
        .apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 1,
                playback: Playback::At(r(1, 2)),
            },
        )
        .expect("a speed");
    assert_eq!(
        project.apply(sequence, Edit::JoinItems { track: 0, index: 0 }),
        Err(ModelStatus::ItemsNotContiguous)
    );
}

#[test]
fn sound_cannot_yet_be_retimed() {
    // Sound at a speed other than one needs a resampler, and a resampler needs
    // a filter somebody chose and a decision about pitch. Playing the samples
    // at the wrong rate in the meantime would be an answer nobody asked for.
    let (mut project, sequence) = project(TrackKind::Audio);
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::At(r(1, 2)),
            },
        ),
        Err(ModelStatus::SoundCannotBeRetimed)
    );
    assert!(
        project
            .apply(
                sequence,
                Edit::SetClipPlayback {
                    track: 0,
                    index: 0,
                    playback: Playback::At(Rational::ONE),
                },
            )
            .is_ok(),
        "and real time is not a retime"
    );
}

#[test]
fn a_gap_has_no_media_to_play_at_any_speed() {
    let (mut project, sequence) = project(TrackKind::Video);
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::gap(frames(10)).expect("a gap"),
            },
        )
        .expect("a gap");
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::At(r(1, 2)),
            },
        ),
        Err(ModelStatus::NotAClip)
    );
}

#[test]
fn a_speed_is_set_by_an_edit_and_undone_by_its_inverse() {
    let (mut project, sequence) = project(TrackKind::Video);
    project
        .apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::At(r(1, 3)),
            },
        )
        .expect("a speed");
    assert_eq!(clip_at(&project, sequence, 0).speed(), Some(r(1, 3)));
    project.undo(sequence).expect("an undo");
    assert_eq!(clip_at(&project, sequence, 0).speed(), Some(Rational::ONE));
    project.redo(sequence).expect("a redo");
    assert_eq!(clip_at(&project, sequence, 0).speed(), Some(r(1, 3)));
}

#[test]
fn a_refused_speed_leaves_the_clip_alone() {
    let (mut project, sequence) = project(TrackKind::Video);
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::At(Rational::ZERO),
            },
        ),
        Err(ModelStatus::SpeedNotUsable)
    );
    assert!(clip_at(&project, sequence, 0).is_real_time());
}

#[test]
fn the_playhead_asks_for_the_retimed_frame() {
    // Through the layer stack, which is what the renderer actually reads.
    use media_editor_core::Instant;

    let (mut project, sequence) = project(TrackKind::Video);
    project
        .apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::At(r(1, 2)),
            },
        )
        .expect("half speed");
    let asked = |tick| {
        project
            .sequence(sequence)
            .expect("a sequence")
            .stack_at(Lane::Picture, Instant::new(tick, RATE))
            .expect("a stack")[0]
            .source()
    };
    assert_eq!(asked(0), 100);
    assert_eq!(asked(1), 100, "the same frame twice, which is slow motion");
    assert_eq!(asked(2), 101);
    assert_eq!(asked(9), 104);
}

#[test]
fn a_retimed_clip_keeps_everything_a_clip_carries() {
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
            fade_out: frames(4),
        },
        Edit::SetClipPlayback {
            track: 0,
            index: 0,
            playback: Playback::At(r(2, 1)),
        },
    ] {
        project.apply(sequence, edit).expect("an edit");
    }
    let held = clip_at(&project, sequence, 0);
    assert_eq!(held.grade(), Some(grade));
    assert_eq!(held.fade_in(), frames(4));
    assert_eq!(held.speed(), Some(r(2, 1)));
    // And a slip keeps the speed, which is the rebuild-through-`new` fault
    // asked of the field added last.
    project
        .apply(
            sequence,
            Edit::SetClipSource {
                track: 0,
                index: 0,
                source_start: 300,
            },
        )
        .expect("a slip");
    assert_eq!(clip_at(&project, sequence, 0).speed(), Some(r(2, 1)));
}

#[test]
fn a_dissolve_over_a_retimed_clip_asks_it_for_the_retimed_frame() {
    // The stack builds a dissolve's two layers down a different arm from an
    // ordinary one, and an arm that reads the in point directly would show a
    // retimed shot at the wrong frame for exactly the length of the dissolve
    // -- a fault that appears only where two clips overlap, which is where
    // nobody is looking at one clip's frame numbers.
    use media_editor_core::Instant;
    use media_editor_model::Transition;

    let (mut project, sequence) = project(TrackKind::Video);
    let media = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track");
    let Item::Clip(first) = media.item(0).expect("an item") else {
        panic!("a clip");
    };
    let second = Item::Clip(Clip::new(first.media(), 500, frames(LENGTH)).expect("a clip"));
    for edit in [
        Edit::InsertItem {
            track: 0,
            index: 1,
            item: second,
        },
        Edit::SetClipPlayback {
            track: 0,
            index: 0,
            playback: Playback::At(r(1, 2)),
        },
        Edit::AddTransition {
            track: 0,
            transition: Transition::new(1, frames(4)).expect("a dissolve"),
        },
    ] {
        project.apply(sequence, edit).expect("an edit");
    }

    // Four frames centred on the cut at LENGTH: 22, 23, 24, 25. The outgoing
    // clip is asked for offsets that run *past its own end*, which is what a
    // handle is, and at half speed those offsets map to half as much media.
    let outgoing = |tick| {
        project
            .sequence(sequence)
            .expect("a sequence")
            .stack_at(Lane::Picture, Instant::new(tick, RATE))
            .expect("a stack")[0]
            .source()
    };
    assert_eq!(
        (22..26).map(outgoing).collect::<Vec<_>>(),
        vec![111, 111, 112, 112],
        "half of the offset past the in point, not the offset"
    );
}

/// A project on a track of `kind` whose media is `length` frames long, with
/// one clip of `over` frames starting at the media's first frame.
fn against(length: i64, over: i64) -> (Project, SequenceId) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("room");
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"short"), RATE, frames(length)).expect("an asset"))
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
                item: Item::Clip(Clip::new(media, 0, frames(over)).expect("a clip")),
            },
        )
        .expect("a clip");
    project.forget_history();
    (project, sequence)
}

#[test]
fn the_span_is_the_two_ticks_a_clip_reads_whichever_way_it_runs() {
    // In order, so a caller checking a range against an asset does not have to
    // know which way the clip runs -- which is the whole reason it exists.
    let held = clip();
    assert_eq!(
        held.source_span().expect("a span"),
        (IN_POINT, IN_POINT + LENGTH - 1),
        "real time reads its own length"
    );
    assert_eq!(
        held.with_speed(r(2, 1))
            .expect("double speed")
            .source_span()
            .expect("a span"),
        (IN_POINT, IN_POINT + 2 * (LENGTH - 1)),
        "and double speed reads about twice it"
    );
    assert_eq!(
        held.with_speed(r(-1, 1))
            .expect("reverse")
            .source_span()
            .expect("a span"),
        (IN_POINT - (LENGTH - 1), IN_POINT),
        "a reverse reads below its in point, and the low end comes first"
    );
}

#[test]
fn a_speed_that_would_read_past_the_end_of_the_media_is_refused() {
    // The media library validates the far end of the retimed source span. A
    // double-speed clip reads twice its timeline duration.
    let (mut project, sequence) = against(100, 60);
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::At(r(2, 1)),
            },
        ),
        Err(ModelStatus::SourceAfterEnd),
        "sixty frames at double speed reads a hundred and eighteen of a hundred"
    );
}

#[test]
fn a_clip_that_reads_the_last_frame_of_its_media_is_accepted() {
    // The bound is inclusive at the last frame, and a guard written with the
    // wrong comparison refuses exactly this clip and nothing else -- so the
    // fixture has to *land* on that frame rather than merely fit.
    //
    // Fifty frames at double speed out of an in point of one read
    // 1, 3, ... 99, and 99 is the last frame of a hundred-frame asset.
    let (mut project, sequence) = against(100, 50);
    project
        .apply(
            sequence,
            Edit::SetClipSource {
                track: 0,
                index: 0,
                source_start: 1,
            },
        )
        .expect("a slip");
    assert!(
        project
            .apply(
                sequence,
                Edit::SetClipPlayback {
                    track: 0,
                    index: 0,
                    playback: Playback::At(r(2, 1)),
                },
            )
            .is_ok(),
        "the last frame of the media is a frame the clip may show"
    );
    // And one frame further along is not, which is the other side of the same
    // comparison: an in point of two reads 2, 4, ... 100.
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipSource {
                track: 0,
                index: 0,
                source_start: 2,
            },
        ),
        Err(ModelStatus::SourceAfterEnd)
    );
}

#[test]
fn a_clip_inserted_already_retimed_is_checked_too() {
    // Setting the speed is one way in and carrying it is the other. A guard on
    // only the first would be a guard a file, a paste or an undo walks around.
    let (mut project, sequence) = against(100, 60);
    let media = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track");
    let Item::Clip(existing) = media.item(0).expect("an item") else {
        panic!("a clip");
    };
    let fast = existing.with_speed(r(2, 1)).expect("double speed");
    assert_eq!(
        project.apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 1,
                item: Item::Clip(fast),
            },
        ),
        Err(ModelStatus::SourceAfterEnd)
    );
}

#[test]
fn lengthening_a_retimed_clip_is_checked_against_what_it_would_read() {
    // A trim that would be legal at real time and is not at double speed. The
    // duration edit and the speed edit reach the same guard from two sides.
    let (mut project, sequence) = against(100, 40);
    project
        .apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::At(r(2, 1)),
            },
        )
        .expect("eighty frames of a hundred");
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetItemDuration {
                track: 0,
                index: 0,
                duration: frames(60),
            },
        ),
        Err(ModelStatus::SourceAfterEnd),
        "sixty at double speed is past the end, though sixty at real time is not"
    );
}
