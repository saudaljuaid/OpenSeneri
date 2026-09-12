// SPDX-License-Identifier: GPL-3.0-only
//! Saving, and the four ways it is allowed to fail.
//!
//! R-9.4: a save is atomic and all-or-nothing, and a save that is interrupted
//! leaves the previous file intact. This file is that rule's negative control.
//! Each test breaks the save at a different step and requires the same thing
//! of every one of them: the committed project is exactly what it was, and it
//! still loads.

use media_editor_abi::seam::{SeamStatus, Slot, Storage};
use media_editor_core::{Digest, Duration, Timebase};
use media_editor_io::{Fault, IoStatus, MemoryStorage, encode, load, save};
use media_editor_model::{Clip, Edit, Item, MediaAsset, Project, TrackKind};

const RATE: Timebase = Timebase::PAL_25;
const CAPACITY: usize = 64 * 1024;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a non-negative length")
}

/// A project with `clips` clips on one track.
fn project_with(clips: usize) -> Project {
    let mut project = Project::new();
    let media = project
        .add_media(
            MediaAsset::new(Digest::of(b"take one"), RATE, frames(50_000)).expect("an asset"),
        )
        .expect("room");
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
    for index in 0..clips {
        let start = i64::try_from(index).unwrap_or(0) * 100;
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 0,
                    index,
                    item: Item::Clip(Clip::new(media, start, frames(90)).expect("a clip")),
                },
            )
            .expect("a clip");
    }
    project
}

#[test]
fn a_saved_project_loads_back_as_itself() {
    let project = project_with(3);
    let mut storage = MemoryStorage::new(CAPACITY);

    let digest = save(&project, &mut storage).expect("a save");
    assert_eq!(storage.commits(), 1);
    assert_eq!(
        digest,
        Digest::of(&encode(&project).expect("an encoding")),
        "the reported digest is the file's"
    );

    let loaded = load(&storage).expect("a load");
    let saved_again = save(&loaded, &mut storage).expect("a save");
    assert_eq!(digest, saved_again, "a load and a save change nothing");
}

#[test]
fn there_is_nothing_to_load_before_anything_is_saved() {
    let storage = MemoryStorage::new(CAPACITY);
    assert_eq!(load(&storage), Err(IoStatus::Seam(SeamStatus::Empty)));
}

/// Save one project, then try to save another with a fault, and require the
/// first to survive whole.
fn the_previous_file_survives(fault: Fault, expected: IoStatus) {
    let first = project_with(2);
    let mut storage = MemoryStorage::new(CAPACITY);
    save(&first, &mut storage).expect("the first save");
    let committed = storage.committed().expect("a committed file").to_vec();
    let commits = storage.commits();

    storage.set_fault(fault);
    let second = project_with(9);
    assert_eq!(
        save(&second, &mut storage),
        Err(expected),
        "the failing save must refuse by name"
    );

    assert_eq!(
        storage.committed().expect("still committed"),
        committed.as_slice(),
        "the project slot changed during a failed save"
    );
    assert_eq!(storage.commits(), commits, "a failed save committed");

    storage.set_fault(Fault::None);
    let recovered = load(&storage).expect("the previous project still loads");
    assert_eq!(
        recovered,
        load_from_bytes(&committed),
        "what loads is what was there before"
    );
    assert_eq!(
        recovered.sequences().len(),
        1,
        "and it is the first project, not the second"
    );
    let sequence = recovered.sequences().iter().next().expect("a sequence").0;
    assert_eq!(
        recovered
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .len(),
        2,
        "two clips, as the first project had"
    );
}

fn load_from_bytes(bytes: &[u8]) -> Project {
    media_editor_io::decode(bytes).expect("the committed bytes are a project")
}

#[test]
fn a_refused_write_leaves_the_previous_file_intact() {
    the_previous_file_survives(Fault::OnWrite, IoStatus::Seam(SeamStatus::Refused));
}

#[test]
fn a_storage_that_stores_something_else_is_caught_before_the_commit() {
    // This is why the save reads back what it wrote. Without that step the
    // corruption would be committed and the previous file would be gone.
    the_previous_file_survives(Fault::Corrupting, IoStatus::WriteNotVerified);
}

#[test]
fn a_refused_read_back_leaves_the_previous_file_intact() {
    the_previous_file_survives(Fault::OnReadBack, IoStatus::Seam(SeamStatus::Refused));
}

#[test]
fn a_refused_commit_leaves_the_previous_file_intact() {
    the_previous_file_survives(Fault::OnCommit, IoStatus::Seam(SeamStatus::Refused));
}

#[test]
fn a_project_too_large_for_the_storage_is_refused_before_the_commit() {
    let first = project_with(1);
    let mut storage = MemoryStorage::new(256);
    save(&first, &mut storage).expect("a small project fits");
    let committed = storage.committed().expect("a file").to_vec();

    let large = project_with(200);
    assert_eq!(
        save(&large, &mut storage),
        Err(IoStatus::Seam(SeamStatus::TooLarge))
    );
    assert_eq!(
        storage.committed().expect("still there"),
        committed.as_slice()
    );
}

#[test]
fn the_scratch_slot_is_the_only_one_a_save_writes() {
    let project = project_with(2);
    let mut storage = MemoryStorage::new(CAPACITY);
    save(&project, &mut storage).expect("a save");

    // After a commit the scratch slot is empty again: the bytes moved rather
    // than being copied, so there is one file and not two that can disagree.
    assert_eq!(storage.len(Slot::Scratch), Err(SeamStatus::Empty));
    assert!(storage.len(Slot::Project).is_ok());
}

#[test]
fn reading_into_something_too_small_is_refused_rather_than_truncated() {
    let project = project_with(2);
    let mut storage = MemoryStorage::new(CAPACITY);
    save(&project, &mut storage).expect("a save");
    let length = storage.len(Slot::Project).expect("a length");

    let mut too_small = std::vec![0_u8; length - 1];
    assert_eq!(
        storage.read(Slot::Project, &mut too_small),
        Err(SeamStatus::TooLarge)
    );

    let mut exact = std::vec![0_u8; length];
    assert_eq!(storage.read(Slot::Project, &mut exact), Ok(length));
}

#[test]
fn saving_repeatedly_converges_on_the_same_bytes() {
    // R-4.1 for the file: the same project produces the same file, every time,
    // so a save that changed nothing leaves a file that changed nothing.
    let project = project_with(5);
    let mut storage = MemoryStorage::new(CAPACITY);
    let first = save(&project, &mut storage).expect("a save");
    let bytes = storage.committed().expect("a file").to_vec();
    for _ in 0..4 {
        let again = save(&project, &mut storage).expect("a save");
        assert_eq!(again, first);
        assert_eq!(storage.committed().expect("a file"), bytes.as_slice());
    }
}
