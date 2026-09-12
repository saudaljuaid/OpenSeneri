// SPDX-License-Identifier: GPL-3.0-only
//! A look on a clip.
//!
//! The model holds the *digest* of a table rather than the table: a cube is
//! 35,937 triples, the model is structure, and `media-editor-render` — where a
//! table lives — sits above the model rather than beside it. Naming it by
//! digest is the same decision the render graph makes about media, for the
//! same reasons: the same grade in two projects is the same grade, a
//! project-local handle would cache it twice, and a file swapped underneath a
//! handle is a different look wearing the same name.
//!
//! Most of what follows is about the grade *travelling* — through a trim,
//! through a split, through a save. A look that fell off a clip when somebody
//! shortened it would be the kind of fault nobody thinks to look for.

use media_editor_core::{Duration, Timebase};
use media_editor_model::media::Digest;
use media_editor_model::{
    Clip, Edit, Item, Lane, MediaAsset, ModelStatus, Project, SequenceId, TrackKind,
};

const RATE: Timebase = Timebase::FILM_24;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a duration")
}

fn look(tag: u8) -> Digest {
    let mut bytes = [0_u8; 32];
    bytes[0] = tag;
    Digest::new(bytes)
}

/// A project with one picture track holding one 48-frame clip.
fn project() -> (Project, SequenceId) {
    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"one"), RATE, frames(240)).expect("an asset"))
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
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 24, frames(48)).expect("a clip")),
            },
        )
        .expect("a clip");
    (project, sequence)
}

/// The grade on one item of the picture track.
fn grade_of(project: &Project, sequence: SequenceId, index: usize) -> Option<Digest> {
    let Item::Clip(clip) = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(index)
        .expect("an item")
    else {
        panic!("not a clip");
    };
    clip.grade()
}

#[test]
fn a_clip_begins_with_no_look_on_it() {
    let (project, sequence) = project();
    assert_eq!(grade_of(&project, sequence, 0), None);
}

#[test]
fn setting_a_grade_undoes_and_redoes() {
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(look(7)),
            },
        )
        .expect("a grade");
    assert_eq!(grade_of(&project, sequence, 0), Some(look(7)));

    // Replacing one look with another, so the inverse has to carry the old one
    // rather than merely knowing there was one.
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(look(9)),
            },
        )
        .expect("a grade");
    assert_eq!(grade_of(&project, sequence, 0), Some(look(9)));

    project.undo(sequence).expect("an undo");
    assert_eq!(
        grade_of(&project, sequence, 0),
        Some(look(7)),
        "undo did not put the previous look back"
    );
    project.undo(sequence).expect("an undo");
    assert_eq!(grade_of(&project, sequence, 0), None);

    project.redo(sequence).expect("a redo");
    project.redo(sequence).expect("a redo");
    assert_eq!(grade_of(&project, sequence, 0), Some(look(9)));
}

#[test]
fn a_grade_survives_a_trim() {
    // A trim is a change of length. A clip that lost its look because somebody
    // shortened it would be the kind of fault nobody thinks to look for — the
    // picture changes at the moment the edit changes, so it reads as the edit.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(look(3)),
            },
        )
        .expect("a grade");
    project
        .apply(
            sequence,
            Edit::SetItemDuration {
                track: 0,
                index: 0,
                duration: frames(20),
            },
        )
        .expect("a trim");
    assert_eq!(grade_of(&project, sequence, 0), Some(look(3)));
}

#[test]
fn a_grade_survives_a_slip() {
    // A slip changes which part of the media a clip uses and nothing else, so
    // it certainly must not change the look.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(look(4)),
            },
        )
        .expect("a grade");
    project
        .apply(
            sequence,
            Edit::SetClipSource {
                track: 0,
                index: 0,
                source_start: 60,
            },
        )
        .expect("a slip");
    assert_eq!(grade_of(&project, sequence, 0), Some(look(4)));
}

#[test]
fn a_split_gives_both_halves_the_look() {
    // Both halves must retain the same grade after a split.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(look(5)),
            },
        )
        .expect("a grade");
    project
        .apply(
            sequence,
            Edit::SplitItem {
                track: 0,
                index: 0,
                offset: 20,
            },
        )
        .expect("a split");
    assert_eq!(grade_of(&project, sequence, 0), Some(look(5)), "the head");
    assert_eq!(grade_of(&project, sequence, 1), Some(look(5)), "the tail");
}

#[test]
fn joining_is_still_the_exact_inverse_of_splitting() {
    // The property the two are built around. Splitting a graded clip and
    // joining it back must give the clip that was there — which needs the
    // grade on both halves, and needs join to keep it.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(look(6)),
            },
        )
        .expect("a grade");
    let before = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item")
        .clone();

    project
        .apply(
            sequence,
            Edit::SplitItem {
                track: 0,
                index: 0,
                offset: 20,
            },
        )
        .expect("a split");
    project
        .apply(sequence, Edit::JoinItems { track: 0, index: 0 })
        .expect("a join");

    let after = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item");
    assert_eq!(*after, before, "a split and a join did not cancel");
}

#[test]
fn two_clips_graded_differently_do_not_join() {
    // Two clips of one piece of media, adjacent in its source and graded
    // differently, are not one item — they are two shots with two looks.
    // Joining them would keep the first's and discard the second's without
    // saying so, so `continues_into` compares the grades and this is refused.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SplitItem {
                track: 0,
                index: 0,
                offset: 20,
            },
        )
        .expect("a split");
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 1,
                grade: Some(look(8)),
            },
        )
        .expect("a grade");

    assert_eq!(
        project.apply(sequence, Edit::JoinItems { track: 0, index: 0 }),
        Err(ModelStatus::ItemsNotContiguous),
        "two looks were joined into one"
    );

    // Give them the same look and they join again, which is what says the
    // refusal is about the looks rather than about anything else the split
    // did.
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(look(8)),
            },
        )
        .expect("a grade");
    project
        .apply(sequence, Edit::JoinItems { track: 0, index: 0 })
        .expect("a join");
    assert_eq!(grade_of(&project, sequence, 0), Some(look(8)));
}

#[test]
fn a_grade_reaches_the_layer_stack() {
    // The stack is what a renderer reads, so a look that stopped at the model
    // would be a look nothing could apply.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(look(11)),
            },
        )
        .expect("a grade");
    let stack = project
        .sequence(sequence)
        .expect("a sequence")
        .stack_at(Lane::Picture, media_editor_core::Instant::new(10, RATE))
        .expect("a stack");
    assert_eq!(stack.len(), 1);
    let graded = stack[0].grade().expect("a look on the layer");
    assert_eq!(graded.look(), look(11));
    assert_eq!(
        graded.strength(),
        media_editor_core::Rational::ONE,
        "a grade nobody animated is all the way on"
    );
}

#[test]
fn a_gap_cannot_be_graded() {
    // A gap has nothing to grade. Accepting it and doing nothing would make
    // the inverse a lie: undoing would have nothing to put back and would
    // still claim to have.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 1,
                item: Item::gap(frames(12)).expect("a gap"),
            },
        )
        .expect("a gap");
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 1,
                grade: Some(look(2)),
            },
        ),
        Err(ModelStatus::NotAClip)
    );
}
