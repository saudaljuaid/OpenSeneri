// SPDX-License-Identifier: GPL-3.0-only
//! The project: the library, the checks it enforces, and stale identifiers.

use media_editor_core::{CoreStatus, Duration, Timebase};
use media_editor_model::media::Digest;
use media_editor_model::{Clip, Edit, Item, MediaAsset, ModelStatus, Project, TrackKind};

const RATE: Timebase = Timebase::PAL_25;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a non-negative length")
}

fn asset(length: i64) -> MediaAsset {
    MediaAsset::new(Digest::new([9; 32]), RATE, frames(length)).expect("an asset")
}

#[test]
fn a_clip_must_fit_inside_its_media() {
    let mut project = Project::new();
    let media = project.add_media(asset(100)).expect("room");
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

    // The last frame of the media is frame 99, so a clip of ten frames may
    // start at 90 and no later. Ranges are half-open everywhere.
    let fits = Item::Clip(Clip::new(media, 90, frames(10)).expect("a clip"));
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: fits,
            },
        )
        .expect("the clip ends exactly at the end of the media");

    let overruns = Item::Clip(Clip::new(media, 91, frames(10)).expect("a clip"));
    assert_eq!(
        project.apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 1,
                item: overruns
            }
        ),
        Err(ModelStatus::SourceAfterEnd)
    );
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .track(0)
            .expect("a track")
            .len(),
        1,
        "the refused insert changed nothing"
    );
}

#[test]
fn a_trim_cannot_run_off_the_end_of_the_media() {
    let mut project = Project::new();
    let media = project.add_media(asset(100)).expect("room");
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
    let item = Item::Clip(Clip::new(media, 90, frames(5)).expect("a clip"));
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item,
            },
        )
        .expect("a clip");

    project
        .apply(
            sequence,
            Edit::SetItemDuration {
                track: 0,
                index: 0,
                duration: frames(10),
            },
        )
        .expect("ten frames from 90 ends exactly at the end");

    assert_eq!(
        project.apply(
            sequence,
            Edit::SetItemDuration {
                track: 0,
                index: 0,
                duration: frames(11)
            }
        ),
        Err(ModelStatus::SourceAfterEnd),
        "a ripple trim is checked against the media, not just against the track"
    );
}

#[test]
fn a_slip_cannot_leave_the_media() {
    let mut project = Project::new();
    let media = project.add_media(asset(100)).expect("room");
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
    let item = Item::Clip(Clip::new(media, 40, frames(10)).expect("a clip"));
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item,
            },
        )
        .expect("a clip");

    project
        .apply(
            sequence,
            Edit::SetClipSource {
                track: 0,
                index: 0,
                source_start: 0,
            },
        )
        .expect("slipping to the head of the media");
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipSource {
                track: 0,
                index: 0,
                source_start: -1
            }
        ),
        Err(ModelStatus::SourceBeforeStart)
    );
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipSource {
                track: 0,
                index: 0,
                source_start: 91
            }
        ),
        Err(ModelStatus::SourceAfterEnd)
    );
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipSource {
                track: 0,
                index: 0,
                source_start: 90
            }
        ),
        Ok(()),
        "ninety is the last position ten frames still fit at"
    );
}

#[test]
fn a_slip_does_not_move_the_clip_or_change_its_length() {
    let mut project = Project::new();
    let media = project.add_media(asset(100)).expect("room");
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
                item: Item::gap(frames(7)).expect("a gap"),
            },
        )
        .expect("a gap");
    let item = Item::Clip(Clip::new(media, 40, frames(10)).expect("a clip"));
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 1,
                item,
            },
        )
        .expect("a clip");

    let before = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item_range(1)
        .expect("a range");
    project
        .apply(
            sequence,
            Edit::SetClipSource {
                track: 0,
                index: 1,
                source_start: 60,
            },
        )
        .expect("a slip");
    let after = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item_range(1)
        .expect("a range");
    assert_eq!(before, after, "a slip is invisible on the timeline");
}

#[test]
fn media_in_another_timebase_is_refused() {
    let mut project = Project::new();
    let film = MediaAsset::new(
        Digest::new([1; 32]),
        Timebase::FILM_24,
        Duration::new(100, Timebase::FILM_24).expect("a length"),
    )
    .expect("an asset");
    let media = project.add_media(film).expect("room");
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

    let item = Item::Clip(Clip::new(media, 0, frames(10)).expect("a clip"));
    assert_eq!(
        project.apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item
            }
        ),
        Err(ModelStatus::MediaTimebaseMismatch),
        "mixed-rate cutting is a later contract, not a silent conversion"
    );
}

#[test]
fn an_asset_description_must_be_in_its_own_timebase() {
    assert_eq!(
        MediaAsset::new(
            Digest::new([0; 32]),
            RATE,
            Duration::new(10, Timebase::FILM_24).expect("a length")
        ),
        Err(ModelStatus::Time(CoreStatus::TimebaseMismatch))
    );
}

#[test]
fn media_still_in_use_cannot_be_removed() {
    let mut project = Project::new();
    let media = project.add_media(asset(100)).expect("room");
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
    let item = Item::Clip(Clip::new(media, 0, frames(10)).expect("a clip"));
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item,
            },
        )
        .expect("a clip");

    assert_eq!(project.remove_media(media), Err(ModelStatus::MediaInUse));

    project.undo(sequence).expect("undo the insert");
    assert!(
        project.remove_media(media).is_ok(),
        "once nothing cuts from it, it can go"
    );
}

#[test]
fn a_removed_asset_leaves_every_identifier_that_named_it_stale() {
    let mut project = Project::new();
    let first = project.add_media(asset(100)).expect("room");
    project.remove_media(first).expect("nothing refers to it");

    // The slot is reused, at a new generation, so the retired identifier does
    // not resolve to the new occupant.
    let second = project.add_media(asset(200)).expect("room");
    assert_eq!(first.index(), second.index(), "the same slot was reused");
    assert_ne!(first.generation(), second.generation());
    assert_eq!(
        project.media().get(first),
        Err(ModelStatus::Time(CoreStatus::StaleIdentifier))
    );
    assert_eq!(
        project
            .media()
            .get(second)
            .expect("the new asset")
            .duration(),
        frames(200)
    );
}

#[test]
fn an_unknown_media_identifier_is_refused_before_anything_changes() {
    let mut project = Project::new();
    let media = project.add_media(asset(100)).expect("room");
    project.remove_media(media).expect("nothing refers to it");
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

    let item = Item::Clip(Clip::new(media, 0, frames(10)).expect("a clip"));
    assert_eq!(
        project.apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item
            }
        ),
        Err(ModelStatus::UnknownMedia)
    );
    assert_eq!(
        project.history().undo_depth(),
        1,
        "only the track was recorded"
    );
}

#[test]
fn a_sequence_is_as_long_as_its_longest_track() {
    let mut project = Project::new();
    let media = project.add_media(asset(1000)).expect("room");
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
                item: Item::Clip(Clip::new(media, 0, frames(30)).expect("a clip")),
            },
        )
        .expect("picture");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 1,
                index: 0,
                item: Item::Clip(Clip::new(media, 0, frames(75)).expect("a clip")),
            },
        )
        .expect("sound");

    assert_eq!(
        project.sequence(sequence).expect("a sequence").duration(),
        Ok(frames(75)),
        "the sound runs past the picture, which is an L-cut, not an error"
    );
}

#[test]
fn a_track_with_items_on_it_cannot_be_removed() {
    let mut project = Project::new();
    let media = project.add_media(asset(100)).expect("room");
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
                item: Item::Clip(Clip::new(media, 0, frames(10)).expect("a clip")),
            },
        )
        .expect("a clip");

    assert_eq!(
        project.apply(sequence, Edit::RemoveTrack { index: 0 }),
        Err(ModelStatus::TrackNotEmpty),
        "removing a track never destroys work in one step"
    );
}
