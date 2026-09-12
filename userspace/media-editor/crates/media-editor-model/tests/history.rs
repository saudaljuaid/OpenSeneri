// SPDX-License-Identifier: GPL-3.0-only
//! Undo and redo.
//!
//! The property this file exists for (R-9.2): for any sequence of edits,
//! undoing all of them reproduces the project exactly, and redoing all of them
//! reproduces the edited project exactly. It is checked here over thousands of
//! generated sessions, from a seeded generator so that a failure is a test
//! case rather than an anecdote (R-4.6).

use media_editor_core::{Duration, Timebase};
use media_editor_model::media::Digest;
use media_editor_model::{
    Clip, Edit, EditJournal, Item, MediaAsset, MediaId, ModelStatus, Project, Sequence, TrackKind,
};

const RATE: Timebase = Timebase::PAL_25;
const MEDIA_FRAMES: i64 = 5000;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a non-negative length")
}

/// A project with one asset and one empty sequence.
fn fresh() -> (Project, MediaId, media_editor_model::SequenceId) {
    let mut project = Project::new();
    let asset =
        MediaAsset::new(Digest::new([3; 32]), RATE, frames(MEDIA_FRAMES)).expect("an asset");
    let media = project.add_media(asset).expect("room in the library");
    let sequence = project.add_sequence(RATE).expect("room for a sequence");
    (project, media, sequence)
}

fn clip(media: MediaId, source_start: i64, length: i64) -> Item {
    Item::Clip(Clip::new(media, source_start, frames(length)).expect("a clip"))
}

/// xorshift64*, so that every generated session is reproducible from its seed.
struct Generator(u64);

impl Generator {
    fn new(seed: u64) -> Self {
        // Zero is the one state this generator cannot leave, so it is not a
        // seed; shifting it away keeps every caller's seed usable.
        Self(seed | 1)
    }

    fn next(&mut self) -> u64 {
        let mut state = self.0;
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        self.0 = state;
        state.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }

    fn below(&mut self, bound: u64) -> u64 {
        if bound == 0 { 0 } else { self.next() % bound }
    }

    fn index(&mut self, bound: usize) -> usize {
        let bound = u64::try_from(bound).unwrap_or(u64::MAX);
        usize::try_from(self.below(bound)).unwrap_or(0)
    }
}

/// Invent one edit against the current shape of a sequence.
fn invent(generator: &mut Generator, sequence: &Sequence, media: MediaId) -> Edit {
    let tracks = sequence.track_count();
    let track = if tracks == 0 {
        0
    } else {
        generator.index(tracks)
    };
    let items = sequence
        .track(track)
        .map_or(0, media_editor_model::Track::len);
    match generator.below(9) {
        0 => Edit::AddTrack {
            index: generator.index(tracks + 1),
            kind: if generator.below(2) == 0 {
                TrackKind::Video
            } else {
                TrackKind::Audio
            },
        },
        1 => Edit::RemoveTrack {
            index: if tracks == 0 {
                0
            } else {
                generator.index(tracks)
            },
        },
        2..=4 => {
            let length = 1 + i64::try_from(generator.below(40)).unwrap_or(1);
            let room = u64::try_from(MEDIA_FRAMES - length).unwrap_or(0);
            let start = i64::try_from(generator.below(room)).unwrap_or(0);
            let item = if generator.below(4) == 0 {
                Item::gap(frames(length)).expect("a gap")
            } else {
                clip(media, start, length)
            };
            Edit::InsertItem {
                track,
                index: generator.index(items + 1),
                item,
            }
        }
        5 => Edit::RemoveItem {
            track,
            index: generator.index(items.max(1)),
        },
        6 => Edit::SetItemDuration {
            track,
            index: generator.index(items.max(1)),
            duration: frames(1 + i64::try_from(generator.below(40)).unwrap_or(1)),
        },
        7 => Edit::SplitItem {
            track,
            index: generator.index(items.max(1)),
            offset: i64::try_from(generator.below(40)).unwrap_or(1),
        },
        _ => Edit::JoinItems {
            track,
            index: generator.index(items.max(1)),
        },
    }
}

/// Run one generated session and check the property.
fn session(seed: u64, steps: usize) -> usize {
    let (mut project, media, id) = fresh();
    let initial = project.sequence(id).expect("a sequence").clone();
    let mut generator = Generator::new(seed);
    let mut applied = 0;

    for _ in 0..steps {
        let edit = invent(
            &mut generator,
            project.sequence(id).expect("a sequence"),
            media,
        );
        // A refused edit is a normal outcome for a generated one, and the rule
        // being checked is that it changed nothing at all.
        let before = project.sequence(id).expect("a sequence").clone();
        match project.apply(id, edit) {
            Ok(()) => applied += 1,
            Err(_) => assert_eq!(
                *project.sequence(id).expect("a sequence"),
                before,
                "a refused edit must leave the sequence untouched"
            ),
        }
    }

    let edited = project.sequence(id).expect("a sequence").clone();

    // Undo everything.
    while project.undo(id).is_ok() {}
    assert_eq!(
        *project.sequence(id).expect("a sequence"),
        initial,
        "undoing every edit of session {seed} did not reproduce the project"
    );
    assert_eq!(project.undo(id), Err(ModelStatus::NothingToDo));

    // Redo everything.
    while project.redo(id).is_ok() {}
    assert_eq!(
        *project.sequence(id).expect("a sequence"),
        edited,
        "redoing every edit of session {seed} did not reproduce the edited project"
    );
    assert_eq!(project.redo(id), Err(ModelStatus::NothingToDo));

    applied
}

#[test]
fn undoing_every_edit_reproduces_the_project() {
    let mut total_applied = 0;
    for seed in 1..=2000_u64 {
        total_applied += session(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15), 24);
    }
    assert!(
        total_applied > 20_000,
        "the generator produced too few accepted edits to prove anything: {total_applied}"
    );
}

#[test]
fn long_sessions_hold_the_property_too() {
    for seed in 1..=40_u64 {
        session(seed.wrapping_mul(0xD1B5_4A32_D192_ED03), 600);
    }
}

#[test]
fn undo_and_redo_walk_the_same_path() {
    let (mut project, media, id) = fresh();
    project
        .apply(
            id,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            id,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: clip(media, 0, 50),
            },
        )
        .expect("a clip");
    project
        .apply(
            id,
            Edit::SplitItem {
                track: 0,
                index: 0,
                offset: 20,
            },
        )
        .expect("a cut");

    let after_three = project.sequence(id).expect("a sequence").clone();
    assert_eq!(project.history().undo_depth(), 3);
    assert_eq!(project.history().redo_depth(), 0);

    project.undo(id).expect("an undo");
    assert_eq!(project.history().undo_depth(), 2);
    assert_eq!(project.history().redo_depth(), 1);
    assert_eq!(
        project
            .sequence(id)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .len(),
        1,
        "the cut is undone, so the clip is whole again"
    );

    project.redo(id).expect("a redo");
    assert_eq!(*project.sequence(id).expect("a sequence"), after_three);
}

#[test]
fn a_new_edit_discards_the_redo_branch() {
    let (mut project, media, id) = fresh();
    project
        .apply(
            id,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            id,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: clip(media, 0, 10),
            },
        )
        .expect("a clip");
    project.undo(id).expect("an undo");
    assert_eq!(project.history().redo_depth(), 1);

    project
        .apply(
            id,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: clip(media, 100, 10),
            },
        )
        .expect("a different clip");
    assert_eq!(
        project.history().redo_depth(),
        0,
        "history keeps one branch, and the new edit is it"
    );
    assert_eq!(project.redo(id), Err(ModelStatus::NothingToDo));
}

#[test]
fn a_journal_notices_when_the_model_changed_behind_its_back() {
    // The negative control for the journal's self-check. Editing a sequence
    // outside its journal makes the recorded inverse no longer describe the
    // model, and the journal must say so rather than carry on.
    let media = {
        let mut project = Project::new();
        let asset =
            MediaAsset::new(Digest::new([1; 32]), RATE, frames(MEDIA_FRAMES)).expect("an asset");
        project.add_media(asset).expect("room")
    };
    let mut sequence = Sequence::new(RATE);
    let mut journal = EditJournal::new();

    journal
        .apply(
            &mut sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    journal
        .apply(
            &mut sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: clip(media, 0, 10),
            },
        )
        .expect("a clip");

    // Reach past the journal and insert something it never saw.
    sequence
        .track_mut(0)
        .expect("a track")
        .insert(0, clip(media, 500, 5))
        .expect("an unrecorded insert");

    assert_eq!(
        journal.undo(&mut sequence),
        Err(ModelStatus::HistoryInconsistent),
        "undo removed the wrong item and the journal caught it"
    );
}

#[test]
fn nothing_to_undo_is_a_named_refusal() {
    let (mut project, _, id) = fresh();
    assert_eq!(project.undo(id), Err(ModelStatus::NothingToDo));
    assert_eq!(project.redo(id), Err(ModelStatus::NothingToDo));
}
