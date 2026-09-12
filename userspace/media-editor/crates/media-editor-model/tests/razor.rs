// SPDX-License-Identifier: GPL-3.0-only
//! Cutting and merging a column of tracks at once.
//!
//! [`Edit::SplitItem`] cuts one item on one track and [`Edit::JoinItems`] puts
//! two back together, and both have been here since the model had items. What
//! they cannot do is the gesture an editor actually makes: a blade dragged
//! down the timeline cuts **every** track it crosses, and dragged back it
//! heals every cut it made.
//!
//! The difference is not convenience, it is **undo**. A razor performed as
//! four separate splits is four entries in the history, and undoing it once
//! leaves three cuts behind — a state nobody edited into existence. So a
//! column is one edit, whose inverse is one edit, over a set of tracks the
//! edit itself carries.

use media_editor_core::{Digest, Duration, Instant, Timebase};
use media_editor_model::{
    Clip, Edit, Item, MediaAsset, ModelStatus, Project, SequenceId, TrackKind, TrackSet, Transition,
};

const RATE: Timebase = Timebase::FILM_24;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a length")
}

fn at(tick: i64) -> Instant {
    Instant::new(tick, RATE)
}

/// Three tracks: two picture, one sound, each one clip of a hundred frames.
fn project(tracks: usize) -> (Project, SequenceId) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("room");
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"footage"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    for index in 0..tracks {
        project
            .apply(
                sequence,
                Edit::AddTrack {
                    index,
                    kind: if index + 1 == tracks {
                        TrackKind::Audio
                    } else {
                        TrackKind::Video
                    },
                },
            )
            .expect("a track");
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: index,
                    index: 0,
                    item: Item::Clip(
                        Clip::new(
                            media,
                            100 * i64::try_from(index).expect("a tick"),
                            frames(100),
                        )
                        .expect("a clip"),
                    ),
                },
            )
            .expect("a clip");
    }
    project.forget_history();
    (project, sequence)
}

fn item_count(project: &Project, sequence: SequenceId, track: usize) -> usize {
    project
        .sequence(sequence)
        .expect("a sequence")
        .track(track)
        .expect("a track")
        .items()
        .len()
}

#[test]
fn a_set_is_a_bit_per_track_and_refuses_one_past_the_bound() {
    let empty = TrackSet::NONE;
    assert!(empty.is_empty());
    assert_eq!(empty.len(), 0);
    assert!(!empty.holds(0));

    let two = empty.with(3).expect("a track").with(0).expect("a track");
    assert!(!two.is_empty());
    assert_eq!(two.len(), 2);
    assert!(two.holds(0) && two.holds(3));
    assert!(!two.holds(1));
    // In index order rather than in the order they were added, which is what
    // makes an edit's effect independent of how its set was built (R-4.5).
    assert_eq!(two.iter().collect::<std::vec::Vec<_>>(), std::vec![0, 3]);
    // Adding a track twice is the same set. A set is a set.
    assert_eq!(two.with(3).expect("a track"), two);

    assert_eq!(
        empty
            .with(media_editor_model::MAX_TRACKS_PER_SEQUENCE)
            .expect_err("a refusal"),
        ModelStatus::UnknownTrack,
        "a set that quietly dropped a track would make a cut and its inverse \
         describe different edits"
    );
}

#[test]
fn the_blade_names_the_tracks_it_would_land_on() {
    let (project, sequence) = project(3);
    let held = project.sequence(sequence).expect("a sequence");

    // Inside every clip: all three.
    let inside = held.cuttable_at(at(40)).expect("a set");
    assert_eq!(
        inside.iter().collect::<std::vec::Vec<_>>(),
        std::vec![0, 1, 2]
    );

    // At the very start every track has a boundary there already, so there is
    // nothing to cut and no refusal either -- a blade on a cut is not a
    // mistake, it is a blade on a cut.
    assert!(held.cuttable_at(at(0)).expect("a set").is_empty());

    // Past the end of every track: nothing has stopped there, it has stopped.
    assert!(held.cuttable_at(at(100)).expect("a set").is_empty());
}

#[test]
fn a_track_that_has_stopped_is_not_in_the_set() {
    // The case a fixture of equal-length tracks cannot see, which is why the
    // tracks here are not equal lengths.
    let (mut project, sequence) = project(2);
    project
        .apply(
            sequence,
            Edit::SetItemDuration {
                track: 1,
                index: 0,
                duration: frames(30),
            },
        )
        .expect("a trim");
    let held = project.sequence(sequence).expect("a sequence");
    assert_eq!(
        held.cuttable_at(at(50))
            .expect("a set")
            .iter()
            .collect::<std::vec::Vec<_>>(),
        std::vec![0],
        "the short track was named at an instant it does not reach"
    );
}

#[test]
fn a_track_under_a_dissolve_is_not_in_the_set() {
    // And the reason is that `Track::split` refuses it. A set that named a
    // track the split refuses would turn a razor into a refusal at the moment
    // somebody dragged it, which is the worst place to find out.
    let (mut project, sequence) = project(2);
    for track in 0..2 {
        project
            .apply(
                sequence,
                Edit::SplitItem {
                    track,
                    index: 0,
                    offset: 50,
                },
            )
            .expect("a cut");
    }
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(10)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");

    let held = project.sequence(sequence).expect("a sequence");
    assert_eq!(
        held.cuttable_at(at(20))
            .expect("a set")
            .iter()
            .collect::<std::vec::Vec<_>>(),
        std::vec![1],
        "the track carrying a dissolve was named"
    );
    // And the model agrees: asking anyway is refused rather than performed.
    let named = TrackSet::NONE.with(0).expect("a track");
    assert_eq!(
        project
            .apply(
                sequence,
                Edit::CutAt {
                    at: at(20),
                    tracks: named
                }
            )
            .expect_err("a refusal"),
        ModelStatus::TransitionInTheWay
    );
}

#[test]
fn a_column_of_cuts_is_one_edit_and_one_undo() {
    // The whole point. Four splits undone once leaves three cuts behind; this
    // undone once leaves none.
    let (mut project, sequence) = project(3);
    let before = project.clone();
    let named = project
        .sequence(sequence)
        .expect("a sequence")
        .cuttable_at(at(40))
        .expect("a set");
    assert_eq!(named.len(), 3);

    project
        .apply(
            sequence,
            Edit::CutAt {
                at: at(40),
                tracks: named,
            },
        )
        .expect("a razor");
    for track in 0..3 {
        assert_eq!(item_count(&project, sequence, track), 2, "track {track}");
    }

    project.undo(sequence).expect("undone");
    for track in 0..3 {
        assert_eq!(
            item_count(&project, sequence, track),
            1,
            "track {track} kept a cut the undo should have healed"
        );
    }
    assert_eq!(
        project.sequence(sequence).expect("a sequence"),
        before.sequence(sequence).expect("a sequence"),
        "undoing a razor did not reproduce the sequence exactly"
    );

    project.redo(sequence).expect("redone");
    for track in 0..3 {
        assert_eq!(item_count(&project, sequence, track), 2, "track {track}");
    }
}

#[test]
fn the_merge_names_exactly_what_the_cut_made() {
    let (mut project, sequence) = project(3);
    let named = project
        .sequence(sequence)
        .expect("a sequence")
        .cuttable_at(at(40))
        .expect("a set");
    project
        .apply(
            sequence,
            Edit::CutAt {
                at: at(40),
                tracks: named,
            },
        )
        .expect("a razor");

    let healable = project
        .sequence(sequence)
        .expect("a sequence")
        .healable_at(at(40))
        .expect("a set");
    assert_eq!(
        healable, named,
        "the set a merge would heal is not the set the cut made"
    );

    project
        .apply(
            sequence,
            Edit::HealAt {
                at: at(40),
                tracks: healable,
            },
        )
        .expect("a merge");
    for track in 0..3 {
        assert_eq!(item_count(&project, sequence, track), 1, "track {track}");
    }
}

#[test]
fn two_shots_that_merely_abut_are_not_healed() {
    // `continues_into` rather than "adjacent", which is the stronger condition
    // and the one that keeps a merge from fusing two different pieces of
    // material into one.
    let (mut project, sequence) = project(1);
    let media = project.media().iter().next().expect("an asset").0;
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 1,
                // A different part of the media, so the two abut on the
                // timeline and do not continue in the source.
                item: Item::Clip(Clip::new(media, 5_000, frames(40)).expect("a clip")),
            },
        )
        .expect("a second shot");

    let held = project.sequence(sequence).expect("a sequence");
    assert!(
        held.healable_at(at(100)).expect("a set").is_empty(),
        "two different shots were named as one shot cut in two"
    );
    let named = TrackSet::NONE.with(0).expect("a track");
    assert_eq!(
        project
            .apply(
                sequence,
                Edit::HealAt {
                    at: at(100),
                    tracks: named
                }
            )
            .expect_err("a refusal"),
        ModelStatus::ItemsNotContiguous
    );
}

#[test]
fn a_column_publishes_every_cut_or_none_of_them() {
    // R-1.4, and the reason the edit is two passes. The set names a track that
    // can be cut and one that cannot, and the refusal must leave the first
    // track exactly as it was rather than half a razor nobody asked for.
    let (mut project, sequence) = project(2);
    project
        .apply(
            sequence,
            Edit::SetItemDuration {
                track: 1,
                index: 0,
                duration: frames(30),
            },
        )
        .expect("a trim");
    // The trim is setup rather than subject, and the assertion at the end of
    // this test is that the *column* left the history alone -- which it could
    // not say while the trim was still sitting in it.
    project.forget_history();
    let before = project.clone();

    let named = TrackSet::NONE
        .with(0)
        .expect("a track")
        .with(1)
        .expect("a track");
    assert_eq!(
        project
            .apply(
                sequence,
                Edit::CutAt {
                    at: at(50),
                    tracks: named
                }
            )
            .expect_err("a refusal"),
        ModelStatus::SplitOutsideItem,
        "the short track cannot be cut at fifty, so the column cannot be"
    );
    assert_eq!(
        project.sequence(sequence).expect("a sequence"),
        before.sequence(sequence).expect("a sequence"),
        "a refused column left a cut behind on the track it could cut"
    );
    // And the history is untouched, so there is nothing to undo.
    assert_eq!(
        project.undo(sequence).expect_err("a refusal"),
        ModelStatus::NothingToDo,
        "a refused edit took a place in the history"
    );
}

#[test]
fn a_column_that_names_nothing_is_refused() {
    // An edit that changes nothing would still take a place in the history and
    // would still claim, on undo, to have put something back.
    let (mut project, sequence) = project(2);
    for edit in [
        Edit::CutAt {
            at: at(40),
            tracks: TrackSet::NONE,
        },
        Edit::HealAt {
            at: at(40),
            tracks: TrackSet::NONE,
        },
    ] {
        assert_eq!(
            project.apply(sequence, edit).expect_err("a refusal"),
            ModelStatus::NothingToDo
        );
    }
}

#[test]
fn a_column_that_names_a_track_the_sequence_does_not_have_is_refused() {
    // Refused rather than skipped: a set that quietly ignored a track would
    // make the inverse describe a different edit from the one performed.
    let (mut project, sequence) = project(2);
    let named = TrackSet::NONE
        .with(0)
        .expect("a track")
        .with(7)
        .expect("a track");
    assert_eq!(
        project
            .apply(
                sequence,
                Edit::CutAt {
                    at: at(40),
                    tracks: named
                }
            )
            .expect_err("a refusal"),
        ModelStatus::UnknownTrack
    );
    assert_eq!(item_count(&project, sequence, 0), 1, "nothing was cut");
}

#[test]
fn a_narrowed_blade_cuts_only_what_it_names() {
    // The same edit with a modifier held down, and it needs no second edit --
    // which is the argument for the set being passed rather than computed.
    let (mut project, sequence) = project(3);
    let named = TrackSet::NONE.with(1).expect("a track");
    project
        .apply(
            sequence,
            Edit::CutAt {
                at: at(40),
                tracks: named,
            },
        )
        .expect("a razor");
    assert_eq!(item_count(&project, sequence, 0), 1);
    assert_eq!(item_count(&project, sequence, 1), 2);
    assert_eq!(item_count(&project, sequence, 2), 1);

    // And its inverse heals that track and only that one.
    project.undo(sequence).expect("undone");
    for track in 0..3 {
        assert_eq!(item_count(&project, sequence, track), 1, "track {track}");
    }
}

#[test]
fn cutting_a_column_twice_and_healing_it_twice_comes_back() {
    // Two blades and two merges, in the other order, which is the property a
    // razor has to have and a sequence of independent splits does not.
    let (mut project, sequence) = project(3);
    let before = project.clone();
    for tick in [30_i64, 70] {
        let named = project
            .sequence(sequence)
            .expect("a sequence")
            .cuttable_at(at(tick))
            .expect("a set");
        project
            .apply(
                sequence,
                Edit::CutAt {
                    at: at(tick),
                    tracks: named,
                },
            )
            .expect("a razor");
    }
    for track in 0..3 {
        assert_eq!(item_count(&project, sequence, track), 3, "track {track}");
    }
    for tick in [30_i64, 70] {
        let named = project
            .sequence(sequence)
            .expect("a sequence")
            .healable_at(at(tick))
            .expect("a set");
        project
            .apply(
                sequence,
                Edit::HealAt {
                    at: at(tick),
                    tracks: named,
                },
            )
            .expect("a merge");
    }
    assert_eq!(
        project.sequence(sequence).expect("a sequence"),
        before.sequence(sequence).expect("a sequence"),
        "two razors and two merges did not come back to the sequence"
    );
}

#[test]
fn a_cut_column_keeps_everything_the_clips_carried() {
    // A razor is a change of *where the cuts are*, and nothing else. The tail
    // of each cut clip has to keep the look, the shape and the animations the
    // clip had -- which is `Item::split`'s business and is exercised here
    // through the column, because the column is a second door to it.
    let (mut project, sequence) = project(1);
    let grade = Digest::of(b"a look");
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(grade),
            },
        )
        .expect("a grade");
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

    let named = TrackSet::NONE.with(0).expect("a track");
    project
        .apply(
            sequence,
            Edit::CutAt {
                at: at(40),
                tracks: named,
            },
        )
        .expect("a razor");
    for index in 0..2 {
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
        assert_eq!(clip.grade(), Some(grade), "item {index} lost its look");
    }
    // The head keeps the fade in and the tail keeps the fade out, which is
    // what a cut through a faded clip means.
    let head = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item")
        .clone();
    let Item::Clip(head) = head else {
        panic!("a clip")
    };
    assert_eq!(head.fade_in(), frames(6));
}

#[test]
fn the_inverse_of_a_merge_is_the_cut_that_made_it() {
    // Both directions, because a razor and a merge are the same edit read two
    // ways and only one of them was reachable from the history until now.
    let (mut project, sequence) = project(2);
    let named = project
        .sequence(sequence)
        .expect("a sequence")
        .cuttable_at(at(40))
        .expect("a set");
    project
        .apply(
            sequence,
            Edit::CutAt {
                at: at(40),
                tracks: named,
            },
        )
        .expect("a razor");
    project.forget_history();
    let cut = project.clone();

    project
        .apply(
            sequence,
            Edit::HealAt {
                at: at(40),
                tracks: named,
            },
        )
        .expect("a merge");
    for track in 0..2 {
        assert_eq!(item_count(&project, sequence, track), 1, "track {track}");
    }
    project.undo(sequence).expect("undone");
    assert_eq!(
        project.sequence(sequence).expect("a sequence"),
        cut.sequence(sequence).expect("a sequence"),
        "undoing a merge did not put the cuts back"
    );
}

#[test]
fn a_merge_publishes_every_heal_or_none_of_them() {
    // The other half of R-1.4 here, and it was missing: the cut had a test
    // that a refused column leaves nothing behind and the *heal* had none, so
    // the control that collapses its two passes into one changed no answer.
    //
    // The set names a track whose boundary is a cut and one whose boundary is
    // two different shots meeting. The second cannot be healed, so neither may
    // be.
    let (mut project, sequence) = project(2);
    let media = project.media().iter().next().expect("an asset").0;

    // Track 0: one shot, cut in two at forty. Healable.
    project
        .apply(
            sequence,
            Edit::SplitItem {
                track: 0,
                index: 0,
                offset: 40,
            },
        )
        .expect("a cut");
    // Track 1: two different shots meeting at forty. Not healable.
    project
        .apply(
            sequence,
            Edit::SetItemDuration {
                track: 1,
                index: 0,
                duration: frames(40),
            },
        )
        .expect("a trim");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 1,
                index: 1,
                item: Item::Clip(Clip::new(media, 5_000, frames(60)).expect("a clip")),
            },
        )
        .expect("a second shot");
    project.forget_history();
    let before = project.clone();

    // The blade agrees with the model about which of the two is a cut.
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .healable_at(at(40))
            .expect("a set")
            .iter()
            .collect::<std::vec::Vec<_>>(),
        std::vec![0],
    );

    let named = TrackSet::NONE
        .with(0)
        .expect("a track")
        .with(1)
        .expect("a track");
    assert_eq!(
        project
            .apply(
                sequence,
                Edit::HealAt {
                    at: at(40),
                    tracks: named
                }
            )
            .expect_err("a refusal"),
        ModelStatus::ItemsNotContiguous
    );
    assert_eq!(
        item_count(&project, sequence, 0),
        2,
        "a refused merge healed the track it could, and published half a merge"
    );
    assert_eq!(
        project.sequence(sequence).expect("a sequence"),
        before.sequence(sequence).expect("a sequence"),
    );
    assert_eq!(
        project.undo(sequence).expect_err("a refusal"),
        ModelStatus::NothingToDo,
        "a refused edit took a place in the history"
    );
}
