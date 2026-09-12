// SPDX-License-Identifier: GPL-3.0-only
//! A fade on a clip: the gesture a cut cannot make.
//!
//! A dissolve sits at a cut and needs two clips. The first item of a
//! programme has nothing before it, so until now there was no way to bring a
//! programme up from black at all — the most ordinary thing an edit does, and
//! the one thing this model could not describe.
//!
//! A fade on the clip is a different thing from a transition at a cut, and the
//! difference is worth stating: a transition is about *two* clips and belongs
//! to the boundary between them; a fade is about one clip and belongs to it.
//! They multiply where they meet, which is a test below.

use media_editor_core::{Digest, Duration, Rational, Timebase};
use media_editor_model::{
    Clip, Edit, Item, Lane, MediaAsset, ModelStatus, Project, SequenceId, TrackKind, Transition,
};

const RATE: Timebase = Timebase::FILM_24;
const LENGTH: i64 = 24;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a length")
}

fn r(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a rational")
}

/// A project with one clip on one picture track.
fn project() -> (Project, SequenceId) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("room");
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"bars"), RATE, frames(9_000)).expect("an asset"))
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

/// A bare clip, out of a project so it has a media identifier to refer to.
fn clip() -> Clip {
    clip_at(&project().0, project().1, 0)
}

#[test]
fn a_clip_nobody_has_faded_is_up_at_every_instant() {
    let held = clip();
    assert!(held.fade_in().is_zero());
    assert!(held.fade_out().is_zero());
    for offset in 0..LENGTH {
        assert_eq!(
            held.fade_at(offset).expect("a fade"),
            Rational::ONE,
            "at {offset}"
        );
    }
}

#[test]
fn a_fade_in_rises_from_nothing_to_all_of_it() {
    // From *nothing*, on the clip's own first frame, and that is the whole
    // difference from a dissolve. A dissolve's fraction never reaches nought
    // or one, because a frame at either end would repeat a neighbour; a fade
    // from black **is** the black, and a first frame that showed the picture
    // would not be a fade from anything.
    let held = clip().with_fades(frames(8), frames(0)).expect("a fade in");
    assert_eq!(held.fade_at(0).expect("a fade"), Rational::ZERO);
    assert_eq!(held.fade_at(2).expect("a fade"), r(1, 4));
    assert_eq!(held.fade_at(4).expect("a fade"), r(1, 2));
    assert_eq!(held.fade_at(8).expect("a fade"), Rational::ONE);
    assert_eq!(held.fade_at(20).expect("a fade"), Rational::ONE);
}

#[test]
fn a_fade_out_falls_from_all_of_it_to_nothing() {
    // And reaches nothing on the clip's *last* frame rather than on the frame
    // after it. The frame after belongs to whatever comes next, and a fade
    // that finished there would leave one frame of picture at the end of every
    // fade to black.
    let held = clip().with_fades(frames(0), frames(8)).expect("a fade out");
    assert_eq!(held.fade_at(LENGTH - 1).expect("a fade"), Rational::ZERO);
    assert_eq!(held.fade_at(LENGTH - 3).expect("a fade"), r(1, 4));
    assert_eq!(held.fade_at(LENGTH - 5).expect("a fade"), r(1, 2));
    assert_eq!(held.fade_at(LENGTH - 9).expect("a fade"), Rational::ONE);
    assert_eq!(held.fade_at(0).expect("a fade"), Rational::ONE);
}

#[test]
fn where_the_two_meet_the_smaller_wins() {
    // A clip faded up and down over its whole length: the two ramps cross in
    // the middle and the answer is whichever is letting less through. Adding
    // them would pass more than the material has; multiplying them would dip
    // in the middle of a clip nobody asked to dip.
    let held = clip().with_fades(frames(12), frames(12)).expect("fades");
    assert_eq!(held.fade_at(0).expect("a fade"), Rational::ZERO);
    assert_eq!(held.fade_at(6).expect("a fade"), r(1, 2));
    assert_eq!(held.fade_at(LENGTH - 1).expect("a fade"), Rational::ZERO);
    let peak = held.fade_at(12).expect("a fade");
    assert_eq!(peak, r(11, 12), "the two ramps cross just short of full");
}

#[test]
fn outside_the_clip_a_fade_is_nothing() {
    // Which happens inside a dissolve's handles, where the offset runs past
    // both ends of the clip. Material before a clip's own start is material
    // before its fade began, and extrapolating the ramp there would give a
    // fraction above one or below nought — neither of which is an amount of
    // anything.
    let held = clip().with_fades(frames(8), frames(8)).expect("fades");
    for offset in [-40_i64, -1, LENGTH, LENGTH + 40] {
        assert_eq!(
            held.fade_at(offset).expect("a fade"),
            Rational::ZERO,
            "at {offset}"
        );
    }
}

#[test]
fn fades_that_together_outlast_the_clip_are_refused() {
    assert!(
        clip().with_fades(frames(12), frames(12)).is_ok(),
        "exactly its length is allowed"
    );
    assert_eq!(
        clip().with_fades(frames(13), frames(12)),
        Err(ModelStatus::FadesLongerThanClip)
    );
    assert_eq!(
        clip().with_fades(frames(LENGTH + 1), frames(0)),
        Err(ModelStatus::FadesLongerThanClip)
    );
}

#[test]
fn a_trim_shorter_than_the_fades_on_it_is_refused() {
    // Clamping them would silently re-time somebody's fade and dropping them
    // would silently remove it. Neither is what a trim was asked to do, and
    // both are the kind of thing nobody notices until a delivery.
    let held = clip().with_fades(frames(10), frames(10)).expect("fades");
    assert_eq!(
        held.with_duration(frames(19)),
        Err(ModelStatus::FadesLongerThanClip)
    );
    assert!(held.with_duration(frames(20)).is_ok());
}

#[test]
fn the_first_item_of_a_programme_can_fade_up() {
    // The gesture this exists for. A transition needs a cut and the first item
    // has none, so the model refuses one there -- and until a fade lived on the
    // clip, that meant a programme could not come up from black.
    let (mut project, sequence) = project();
    // A transition is refused before it is even offered to a track: a cut is
    // named by the item after it, and there is no cut before the first item.
    assert_eq!(
        Transition::new(0, frames(8)).err(),
        Some(ModelStatus::UnknownItem),
        "there is no cut before the first item"
    );
    project
        .apply(
            sequence,
            Edit::SetClipFades {
                track: 0,
                index: 0,
                fade_in: frames(8),
                fade_out: frames(0),
            },
        )
        .expect("a fade up from black");
    assert_eq!(clip_at(&project, sequence, 0).fade_in(), frames(8));
}

#[test]
fn a_fade_is_set_by_an_edit_and_undone_by_its_inverse() {
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipFades {
                track: 0,
                index: 0,
                fade_in: frames(6),
                fade_out: frames(4),
            },
        )
        .expect("fades");
    assert_eq!(clip_at(&project, sequence, 0).fade_out(), frames(4));
    project.undo(sequence).expect("an undo");
    assert!(clip_at(&project, sequence, 0).fade_in().is_zero());
    project.redo(sequence).expect("a redo");
    assert_eq!(clip_at(&project, sequence, 0).fade_in(), frames(6));
}

#[test]
fn a_gap_cannot_be_faded() {
    // It is already nothing.
    let (mut project, sequence) = project();
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
            Edit::SetClipFades {
                track: 0,
                index: 0,
                fade_in: frames(4),
                fade_out: frames(0),
            },
        ),
        Err(ModelStatus::NotAClip)
    );
}

#[test]
fn a_refused_fade_leaves_the_clip_alone() {
    let (mut project, sequence) = project();
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipFades {
                track: 0,
                index: 0,
                fade_in: frames(LENGTH),
                fade_out: frames(LENGTH),
            },
        ),
        Err(ModelStatus::FadesLongerThanClip)
    );
    assert!(clip_at(&project, sequence, 0).fade_in().is_zero());
}

#[test]
fn a_fade_survives_a_slip_and_a_roll() {
    // The fault found three times over, asked of the field added last: a
    // rebuild through `Clip::new` drops what it was not told about, and both
    // of these rebuild a clip from a length and a source position.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipFades {
                track: 0,
                index: 0,
                fade_in: frames(6),
                fade_out: frames(6),
            },
        )
        .expect("fades");
    project
        .apply(
            sequence,
            Edit::SetClipSource {
                track: 0,
                index: 0,
                source_start: 200,
            },
        )
        .expect("a slip");
    assert_eq!(clip_at(&project, sequence, 0).fade_in(), frames(6));
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 1,
                item: Item::Clip(
                    Clip::new(clip_at(&project, sequence, 0).media(), 400, frames(LENGTH))
                        .expect("a clip"),
                ),
            },
        )
        .expect("a second clip");
    project
        .apply(
            sequence,
            Edit::RollCut {
                track: 0,
                boundary: 1,
                by: 2,
            },
        )
        .expect("a roll");
    assert_eq!(clip_at(&project, sequence, 0).fade_out(), frames(6));
}

#[test]
fn the_layer_stack_carries_the_fade_and_where_it_is_going() {
    use media_editor_core::Instant;

    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipFades {
                track: 0,
                index: 0,
                fade_in: frames(8),
                fade_out: frames(0),
            },
        )
        .expect("a fade in");
    let stack = |tick| {
        project
            .sequence(sequence)
            .expect("a sequence")
            .stack_at(Lane::Picture, Instant::new(tick, RATE))
            .expect("a stack")
    };
    assert_eq!(stack(0)[0].fade(), Rational::ZERO);
    assert_eq!(stack(0)[0].fade_arriving(), r(1, 8));
    assert_eq!(stack(4)[0].fade(), r(1, 2));
    assert_eq!(stack(4)[0].fade_arriving(), r(5, 8));
    assert_eq!(
        stack(4)[0].opacity(),
        Rational::ONE,
        "the track is doing nothing, which is a different thing from the clip"
    );
}

#[test]
fn a_clip_that_simply_ends_is_not_a_clip_fading_out() {
    // The pair of fades a block of sound needs is *this clip's* fade one tick
    // on, not whatever the timeline shows there. Reading the next frame off
    // the track instead made every unfaded clip duck to silence over its last
    // block -- and the mixdown tests said so within a minute of it being
    // written, which is the only reason it is not in the file.
    use media_editor_core::Instant;

    let (project, sequence) = project();
    let stack = project
        .sequence(sequence)
        .expect("a sequence")
        .stack_at(Lane::Picture, Instant::new(LENGTH - 1, RATE))
        .expect("a stack");
    assert_eq!(stack[0].fade(), Rational::ONE, "the last frame is up");
    assert_eq!(
        stack[0].fade_arriving(),
        Rational::ONE,
        "and it is still up at the sample after it"
    );
}
