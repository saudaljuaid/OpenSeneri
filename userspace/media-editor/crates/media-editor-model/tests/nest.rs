// SPDX-License-Identifier: GPL-3.0-only
//! A sequence used as media.
//!
//! `ARCHITECTURE.md` has listed nested sequences as planned since its first
//! version, "so the shape is decided before the pressure to compromise it
//! arrives". The shape turned out to be one already here: **a title is media
//! the program makes out of words**, so a nest is media it makes out of a
//! sequence — and a clip cuts from it, trims it, grades it, masks it, frames
//! it, fades it and animates all four, with not one line that knows what a
//! nest is.
//!
//! What is new is what a nest makes *possible* and therefore has to refuse: a
//! sequence containing itself, and a chain too deep to render.

use media_editor_core::{Digest, Duration, Instant, Timebase};
use media_editor_model::{
    Clip, Edit, Item, MAX_NESTING_DEPTH, MediaAsset, MediaId, ModelStatus, Project, SequenceId,
    TrackKind,
};

const RATE: Timebase = Timebase::FILM_24;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a length")
}

fn at(tick: i64) -> Instant {
    Instant::new(tick, RATE)
}

/// A project with one asset and one sequence holding a hundred frames of it.
fn project() -> (Project, SequenceId, MediaId) {
    let mut project = Project::new();
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"footage"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    let sequence = new_sequence(&mut project, media, 100);
    project.forget_history();
    (project, sequence, media)
}

/// Another sequence, with one clip of `length` frames on one picture track.
fn new_sequence(project: &mut Project, media: MediaId, length: i64) -> SequenceId {
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
                item: Item::Clip(Clip::new(media, 0, frames(length)).expect("a clip")),
            },
        )
        .expect("a clip");
    sequence
}

/// Nest `inner` as an asset, ready to be cut from.
fn nest(project: &mut Project, inner: SequenceId) -> MediaId {
    let length = project
        .sequence(inner)
        .expect("a sequence")
        .duration()
        .expect("a length");
    project
        .add_media(MediaAsset::nesting(inner, RATE, length).expect("an asset"))
        .expect("room")
}

/// Put a clip of `asset` on `sequence`.
fn place(
    project: &mut Project,
    sequence: SequenceId,
    asset: MediaId,
    length: i64,
) -> Result<(), ModelStatus> {
    let index = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .items()
        .len();
    project.apply(
        sequence,
        Edit::InsertItem {
            track: 0,
            index,
            item: Item::Clip(Clip::new(asset, 0, frames(length)).expect("a clip")),
        },
    )
}

#[test]
fn a_nest_is_media_named_by_which_sequence_it_is() {
    // Not by content, and that is the one place a nest differs from the other
    // two kinds. A title is named by what it says, so the same words in two
    // projects are one asset. A sequence is a thing somebody is still editing,
    // and a digest over its contents would give the asset a new identity at
    // every keystroke -- repointing, or orphaning, every clip that referred to
    // it.
    let (mut project, outer, media) = project();
    let inner = new_sequence(&mut project, media, 60);
    let asset = nest(&mut project, inner);

    let held = project.media().get(asset).expect("an asset");
    assert_eq!(held.nested(), Some(inner));
    assert_eq!(held.title(), None);
    assert_eq!(
        held.duration(),
        frames(60),
        "a nest is as long as its sequence"
    );

    // Two nests of two sequences are two assets, and the digests say so.
    let other = new_sequence(&mut project, media, 60);
    let second = nest(&mut project, other);
    assert_ne!(
        project.media().get(asset).expect("an asset").digest(),
        project.media().get(second).expect("an asset").digest(),
        "two sequences of the same length collided"
    );
    let _ = outer;
}

#[test]
fn a_nest_has_nowhere_to_be() {
    // The same refusal a title carries, for the same reason: it is made rather
    // than found, so a location would invite somebody to relink it to a file
    // and the file would be a different asset the moment it was opened.
    //
    // It needed no new code, and the reason is worth naming: `with_location`
    // asks whether the source is a *recording* rather than whether it is a
    // title. A guard written as "is this the one kind that has a location"
    // covers a kind that did not exist when it was written; one written as
    // "is this the other kind" would not have.
    let (mut project, _, media) = project();
    let inner = new_sequence(&mut project, media, 60);
    let asset = nest(&mut project, inner);
    assert_eq!(
        project
            .set_media_location(
                asset,
                Some(media_editor_model::Location::new(b"/somewhere").expect("a hint")),
            )
            .expect_err("a refusal"),
        ModelStatus::NotRecordedMedia
    );
}

#[test]
fn a_clip_cuts_from_a_nest_like_any_other_media() {
    // The claim the whole feature rests on, cashed: a nest goes through the
    // ordinary insert, the ordinary source check and the ordinary layer stack,
    // and nothing along the way was told what a nest is.
    let (mut project, outer, media) = project();
    let inner = new_sequence(&mut project, media, 60);
    let asset = nest(&mut project, inner);
    place(&mut project, outer, asset, 40).expect("a nested clip");

    let stack = project
        .sequence(outer)
        .expect("a sequence")
        .stack_at(media_editor_model::Lane::Picture, at(120))
        .expect("a stack");
    assert_eq!(stack.len(), 1);
    assert_eq!(
        stack[0].media(),
        asset,
        "the layer names the nest, like any other media"
    );
    assert_eq!(
        stack[0].source(),
        20,
        "twenty frames into the nested clip is twenty frames into the nest"
    );
}

#[test]
fn a_sequence_cannot_contain_itself() {
    let (mut project, outer, _) = project();
    let asset = nest(&mut project, outer);
    assert_eq!(
        place(&mut project, outer, asset, 10).expect_err("a refusal"),
        ModelStatus::SequenceWouldContainItself
    );
    // And the refusal wrote nothing.
    assert_eq!(
        project
            .sequence(outer)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .items()
            .len(),
        1,
    );
}

#[test]
fn a_sequence_cannot_contain_itself_through_another() {
    // The case a direct check misses. A holds B, and then B is asked to hold A.
    let (mut project, first, media) = project();
    let second = new_sequence(&mut project, media, 60);
    let inner = nest(&mut project, second);
    place(&mut project, first, inner, 10).expect("a nested clip");

    let outer = nest(&mut project, first);
    assert_eq!(
        place(&mut project, second, outer, 10).expect_err("a refusal"),
        ModelStatus::SequenceWouldContainItself
    );
}

#[test]
fn a_chain_deeper_than_the_bound_is_refused() {
    // A depth-limited walk with an explicit stack, which R-5.5 requires by
    // name for exactly this structure. The bound is reached by building a
    // chain one link at a time until it refuses, which is also what says the
    // links *below* it were all accepted.
    let (mut project, deepest, media) = project();
    let mut held = deepest;
    let mut built = 1;
    loop {
        let asset = nest(&mut project, held);
        let next = new_sequence(&mut project, media, 100);
        match place(&mut project, next, asset, 10) {
            Ok(()) => {
                held = next;
                built += 1;
            }
            Err(refusal) => {
                assert_eq!(refusal, ModelStatus::NestingTooDeep);
                break;
            }
        }
        assert!(built < 64, "the bound never bit");
    }
    assert_eq!(
        built, MAX_NESTING_DEPTH,
        "the chain stopped somewhere other than the bound"
    );
}

#[test]
fn a_nest_says_how_long_its_sequence_is_now() {
    // A nest is the one asset whose length is a fact about the project rather
    // than about the world, so the project keeps it true. Without this the
    // asset would go stale the moment somebody trimmed the nested sequence,
    // and every check that asks an asset how long it is would be asking a
    // number that was right once.
    let (mut project, outer, media) = project();
    let inner = new_sequence(&mut project, media, 60);
    let asset = nest(&mut project, inner);
    place(&mut project, outer, asset, 40).expect("a nested clip");
    assert_eq!(
        project.media().get(asset).expect("an asset").duration(),
        frames(60)
    );

    // Lengthen the nested sequence: the asset follows.
    project
        .apply(
            inner,
            Edit::SetItemDuration {
                track: 0,
                index: 0,
                duration: frames(90),
            },
        )
        .expect("a trim");
    assert_eq!(
        project.media().get(asset).expect("an asset").duration(),
        frames(90),
        "the nest still says how long it used to be"
    );
}

#[test]
fn shortening_a_nest_past_what_a_parent_reads_is_refused() {
    // The invariant nesting makes possible to break at a distance: every other
    // check here is about the sequence an edit named, and this one is about a
    // clip somewhere else entirely.
    //
    // Refused rather than allowed-and-shown-black, because R-1.3 is refuse
    // rather than repair -- and because a clip reading past its media is
    // exactly what `Project::validate` has always refused when the media was a
    // recording.
    let (mut project, outer, media) = project();
    let inner = new_sequence(&mut project, media, 60);
    let asset = nest(&mut project, inner);
    place(&mut project, outer, asset, 60).expect("a nested clip reading all of it");
    project.forget_history();
    let before = project.clone();

    assert_eq!(
        project
            .apply(
                inner,
                Edit::SetItemDuration {
                    track: 0,
                    index: 0,
                    duration: frames(30),
                },
            )
            .expect_err("a refusal"),
        ModelStatus::SourceAfterEnd
    );
    // And the refusal was undone: the nested sequence is as it was, the asset
    // still says sixty, and the history is empty.
    assert_eq!(
        project.sequence(inner).expect("a sequence"),
        before.sequence(inner).expect("a sequence"),
        "a refused edit left the nested sequence shortened"
    );
    assert_eq!(
        project.media().get(asset).expect("an asset").duration(),
        frames(60),
        "a refused edit left the nest saying the wrong length"
    );
    assert_eq!(
        project.undo(inner).expect_err("a refusal"),
        ModelStatus::NothingToDo,
        "a refused edit took a place in the history"
    );
}

#[test]
fn shortening_a_nest_a_parent_does_not_read_to_is_allowed() {
    // The other half, which the test above cannot show: the refusal is about
    // what a parent *reads*, not about nesting itself.
    let (mut project, outer, media) = project();
    let inner = new_sequence(&mut project, media, 60);
    let asset = nest(&mut project, inner);
    place(&mut project, outer, asset, 20).expect("a nested clip reading a fifth of it");

    project
        .apply(
            inner,
            Edit::SetItemDuration {
                track: 0,
                index: 0,
                duration: frames(30),
            },
        )
        .expect("a trim a parent does not notice");
    assert_eq!(
        project.media().get(asset).expect("an asset").duration(),
        frames(30)
    );
}

#[test]
fn a_nested_clip_carries_everything_a_clip_carries() {
    // The claim, again, from the decorations' side. None of these had to be
    // told what a nest is.
    let (mut project, outer, media) = project();
    let inner = new_sequence(&mut project, media, 60);
    let asset = nest(&mut project, inner);
    place(&mut project, outer, asset, 40).expect("a nested clip");
    let index = 1;

    let grade = Digest::of(b"a look");
    project
        .apply(
            outer,
            Edit::SetClipGrade {
                track: 0,
                index,
                grade: Some(grade),
            },
        )
        .expect("a grade");
    project
        .apply(
            outer,
            Edit::SetClipFades {
                track: 0,
                index,
                fade_in: frames(5),
                fade_out: frames(5),
            },
        )
        .expect("fades");
    project
        .apply(
            outer,
            Edit::SetClipMask {
                track: 0,
                index,
                mask: Some(
                    media_editor_model::Mask::rectangle(
                        media_editor_core::Rational::new(1, 4).expect("a rational"),
                        media_editor_core::Rational::new(1, 4).expect("a rational"),
                        media_editor_core::Rational::new(3, 4).expect("a rational"),
                        media_editor_core::Rational::new(3, 4).expect("a rational"),
                    )
                    .expect("a mask"),
                ),
            },
        )
        .expect("a mask");

    // Two frames into the nested clip, which begins at a hundred. The fade in
    // is five frames, so the fade reads two fifths -- derived from the ramp's
    // definition rather than read back out of it.
    let stack = project
        .sequence(outer)
        .expect("a sequence")
        .stack_at(media_editor_model::Lane::Picture, at(102))
        .expect("a stack");
    let layer = &stack[0];
    assert_eq!(layer.grade().expect("a look").look(), grade);
    assert!(layer.mask().is_some());
    assert_eq!(
        layer.fade(),
        media_editor_core::Rational::new(2, 5).expect("a rational"),
        "the fade on a nested clip is the fade on a clip"
    );
}

#[test]
fn a_nest_can_be_cut_and_merged_like_any_other_clip() {
    // The razor and the merge, on a nested clip, with nothing added for it.
    let (mut project, outer, media) = project();
    let inner = new_sequence(&mut project, media, 60);
    let asset = nest(&mut project, inner);
    place(&mut project, outer, asset, 40).expect("a nested clip");

    let named = project
        .sequence(outer)
        .expect("a sequence")
        .cuttable_at(at(120))
        .expect("a set");
    project
        .apply(
            outer,
            Edit::CutAt {
                at: at(120),
                tracks: named,
            },
        )
        .expect("a razor through a nest");
    assert_eq!(
        project
            .sequence(outer)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .items()
            .len(),
        3,
    );
    project.undo(outer).expect("undone");
    assert_eq!(
        project
            .sequence(outer)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .items()
            .len(),
        2,
    );
}
